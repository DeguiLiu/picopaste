/**
 * MIT License
 *
 * Copyright (c) 2026 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file clipboard.cpp
 * @brief Win32 clipboard capture to one temporary file.
 *
 * Layout of this file:
 *   1. little-endian readers and a DIB geometry parser
 *   2. DibSource: a minimal IWICBitmapSource that reads the locked DIB in place
 *   3. the WIC PNG encode path used for CF_DIBV5 / CF_DIB
 *   4. the clipboard capture entry points (PNG fast path + DIB fallback)
 *
 * On the DIB path the pixels are NEVER copied into our heap. A 4K screenshot is
 * ~33 MB uncompressed and would alone blow the memory budget; DibSource points
 * straight at the locked HGLOBAL and the encoder pulls scanlines through it.
 *
 * The design document named IWICImagingFactory::CreateBitmapFromMemory here.
 * That call could not be used: its documentation does not promise to wrap the
 * caller's buffer rather than copy it (the returned IWICBitmap is lockable and
 * writable, i.e. it owns storage), and it cannot express a bottom-up DIB -- the
 * usual shape for CF_DIB -- without a full-image vertical flip. DibSource gives
 * true zero-copy reads and handles bottom-up rows and indexed palettes by
 * mapping each destination scanline to its real source row.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "clipboard.hpp"

#include "win32_util.hpp"

#include <cstring>

#include <objbase.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wincodec.h>

namespace picopaste::win32 {
namespace {

// ---------------------------------------------------------------------------
// 1. DIB parsing
// ---------------------------------------------------------------------------

constexpr std::uint32_t kBitmapInfoHeaderSize = 40u;
constexpr std::uint32_t kBitmapV4HeaderSize = 108u;
constexpr std::uint32_t kBiRgb = 0u;
constexpr std::uint32_t kBiBitfields = 3u;

// A clipboard fetch is retried over this bounded window. Both values stay small
// because the clipboard is held open for the whole retry.
constexpr std::int32_t kFetchAttempts = 10;
constexpr DWORD kFetchRetryMs = 10;

std::uint16_t ReadU16(const std::uint8_t* p) noexcept {
  std::uint16_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::uint32_t ReadU32(const std::uint8_t* p) noexcept {
  std::uint32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

std::int32_t ReadI32(const std::uint8_t* p) noexcept {
  std::int32_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

struct Channel {
  std::uint32_t mask = 0;
  std::uint32_t shift = 0;
  std::uint32_t bits = 0;
};

// Derive shift and contiguous bit width from a channel mask. Returns false for
// a non-contiguous or zero mask, which means the DIB uses a layout we would
// decode wrongly.
bool DescribeChannel(std::uint32_t mask, Channel* out) noexcept {
  if (0u == mask) {
    return false;
  }
  std::uint32_t shift = 0;
  while (shift < 32u && ((mask >> shift) & 1u) == 0u) {
    ++shift;
  }
  std::uint32_t bits = 0;
  while ((shift + bits) < 32u && ((mask >> (shift + bits)) & 1u) == 1u) {
    ++bits;
  }
  // Reconstruct the mask from shift/bits and require an exact match: a gap in
  // the mask is not something our per-pixel extraction can represent.
  const std::uint32_t reconstructed = (((bits == 32u) ? 0xFFFFFFFFu : ((1u << bits) - 1u)) << shift);
  if (reconstructed != mask) {
    return false;
  }
  out->mask = mask;
  out->shift = shift;
  out->bits = bits;
  return true;
}

struct DibGeometry {
  UINT width = 0;
  UINT height = 0;
  UINT bitcount = 0;
  UINT stride = 0;
  bool top_down = false;
  bool indexed = false;
  bool has_alpha = false;
  const std::uint8_t* pixels = nullptr;
  const std::uint8_t* palette = nullptr;
  UINT palette_entries = 0;
  Channel red{};
  Channel green{};
  Channel blue{};
  Channel alpha{};
};

// Read and validate the BITMAPINFO header into `geo`. Outputs the header size,
// the compression tag, and the palette entry count for the layout resolver.
bool ParseDibHeader(const std::uint8_t* dib, std::size_t dib_bytes, DibGeometry* geo, std::uint32_t* header_size,
                    std::uint32_t* compression, std::uint32_t* clr_used) noexcept {
  if (nullptr == dib || nullptr == geo || dib_bytes < kBitmapInfoHeaderSize) {
    return false;
  }
  *header_size = ReadU32(dib);
  if (*header_size < kBitmapInfoHeaderSize || *header_size > dib_bytes) {
    return false;
  }

  const std::int32_t raw_height = ReadI32(dib + 8);
  geo->width = static_cast<UINT>(ReadI32(dib + 4));
  geo->bitcount = ReadU16(dib + 14);
  *compression = ReadU32(dib + 16);
  *clr_used = ReadU32(dib + 32);
  if (geo->width == 0u || raw_height == 0) {
    return false;
  }
  geo->top_down = raw_height < 0;
  geo->height = static_cast<UINT>(raw_height < 0 ? -raw_height : raw_height);
  return true;
}

// Derive the shift/width of the red, green, blue and (optional) alpha channels.
bool DescribeMasks(DibGeometry* geo, std::uint32_t red_mask, std::uint32_t green_mask, std::uint32_t blue_mask,
                   std::uint32_t alpha_mask) noexcept {
  if (DescribeChannel(red_mask, &geo->red) == false || DescribeChannel(green_mask, &geo->green) == false ||
      DescribeChannel(blue_mask, &geo->blue) == false) {
    return false;
  }
  if (alpha_mask != 0u) {
    if (DescribeChannel(alpha_mask, &geo->alpha) == false) {
      return false;
    }
    geo->has_alpha = true;
  }
  return true;
}

// Resolve the palette, colour masks and pixel offset for the header's layout.
// Returns false for a layout we would decode incorrectly.
bool ResolveDibLayout(const std::uint8_t* dib, std::uint32_t header_size, std::uint32_t compression,
                      std::uint32_t clr_used, DibGeometry* geo, std::uint32_t* pixel_offset) noexcept {
  const bool bitfields = (compression == kBiBitfields);

  // Colour masks: present inside the header from BITMAPV4HEADER on, otherwise
  // (BITMAPINFOHEADER + BI_BITFIELDS) they follow the 40-byte header.
  std::uint32_t red_mask = 0;
  std::uint32_t green_mask = 0;
  std::uint32_t blue_mask = 0;
  std::uint32_t alpha_mask = 0;
  std::uint32_t trailing_mask_bytes = 0;
  if (header_size >= kBitmapV4HeaderSize && bitfields) {
    red_mask = ReadU32(dib + 40);
    green_mask = ReadU32(dib + 44);
    blue_mask = ReadU32(dib + 48);
    alpha_mask = ReadU32(dib + 52);
  } else if (header_size == kBitmapInfoHeaderSize && bitfields) {
    red_mask = ReadU32(dib + 40);
    green_mask = ReadU32(dib + 44);
    blue_mask = ReadU32(dib + 48);
    trailing_mask_bytes = 12u;
  }

  geo->indexed = (geo->bitcount <= 8u) && (compression == kBiRgb);
  if (geo->bitcount <= 8u && compression != kBiRgb) {
    // Indexed images must be BI_RGB; BI_BITFIELDS on a <=8bpp image is not a
    // layout we decode.
    return false;
  }

  std::uint32_t palette_bytes = 0;
  if (geo->indexed) {
    geo->palette_entries = (clr_used != 0u) ? clr_used : (1u << geo->bitcount);
    palette_bytes = geo->palette_entries * 4u;
    geo->palette = dib + header_size;
  }

  *pixel_offset = header_size + trailing_mask_bytes + palette_bytes;

  // Supported direct-colour layouts. BI_RGB 16/24/32 and BI_BITFIELDS 16/32.
  if (geo->indexed) {
    if (geo->bitcount != 1u && geo->bitcount != 4u && geo->bitcount != 8u) {
      return false;
    }
  } else if (geo->bitcount == 24u && compression == kBiRgb) {
    red_mask = 0x00FF0000u;
    green_mask = 0x0000FF00u;
    blue_mask = 0x000000FFu;
  } else if (geo->bitcount == 32u && (compression == kBiRgb || bitfields)) {
    if (compression == kBiRgb) {
      red_mask = 0x00FF0000u;
      green_mask = 0x0000FF00u;
      blue_mask = 0x000000FFu;
      alpha_mask = 0u;
    }
  } else if (geo->bitcount == 16u && (compression == kBiRgb || bitfields)) {
    if (compression == kBiRgb) {
      red_mask = 0x7C00u;  // X1R5G5B5
      green_mask = 0x03E0u;
      blue_mask = 0x001Fu;
    }
  } else {
    return false;
  }

  if (geo->indexed == false && DescribeMasks(geo, red_mask, green_mask, blue_mask, alpha_mask) == false) {
    return false;
  }
  return true;
}

bool ParseDib(const std::uint8_t* dib, std::size_t dib_bytes, DibGeometry* geo) noexcept {
  std::uint32_t header_size = 0;
  std::uint32_t compression = 0;
  std::uint32_t clr_used = 0;
  if (ParseDibHeader(dib, dib_bytes, geo, &header_size, &compression, &clr_used) == false) {
    return false;
  }

  std::uint32_t pixel_offset = 0;
  if (ResolveDibLayout(dib, header_size, compression, clr_used, geo, &pixel_offset) == false) {
    return false;
  }

  const std::uint64_t stride64 = ((static_cast<std::uint64_t>(geo->width) * geo->bitcount + 31u) / 32u) * 4u;
  const std::uint64_t needed = static_cast<std::uint64_t>(pixel_offset) + stride64 * geo->height;
  if (needed > dib_bytes) {
    return false;
  }
  geo->stride = static_cast<UINT>(stride64);
  geo->pixels = dib + pixel_offset;
  return true;
}

std::uint8_t ScaleChannel(std::uint32_t value, std::uint32_t bits) noexcept {
  if (0u == bits) {
    return 0u;
  }
  if (bits >= 8u) {
    return static_cast<std::uint8_t>(value & 0xFFu);
  }
  const std::uint32_t max = (1u << bits) - 1u;
  return static_cast<std::uint8_t>((value * 255u + max / 2u) / max);
}

std::uint8_t ExtractChannel(std::uint32_t pixel, const Channel& ch) noexcept {
  return ScaleChannel((pixel & ch.mask) >> ch.shift, ch.bits);
}

// ---------------------------------------------------------------------------
// 2. DibSource -- an IWICBitmapSource that reads the locked DIB in place.
// ---------------------------------------------------------------------------

class DibSource final : public IWICBitmapSource {
 public:
  DibSource() noexcept = default;

  void Init(const DibGeometry& geo) noexcept { geo_ = geo; }

  // IUnknown ----------------------------------------------------------------
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) {
      return E_INVALIDARG;
    }
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IWICBitmapSource)) {
      *ppv = static_cast<IWICBitmapSource*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_count_)); }

  ULONG STDMETHODCALLTYPE Release() override {
    // Stack-allocated helper: this never deletes. It still ref-counts so the
    // caller can combine it with an owning WIC object safely.
    return static_cast<ULONG>(InterlockedDecrement(&ref_count_));
  }

  // IWICBitmapSource --------------------------------------------------------
  HRESULT STDMETHODCALLTYPE GetSize(UINT* width, UINT* height) override {
    if (width == nullptr || height == nullptr) {
      return E_INVALIDARG;
    }
    *width = geo_.width;
    *height = geo_.height;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* format) override {
    if (nullptr == format) {
      return E_INVALIDARG;
    }
    *format = GUID_WICPixelFormat32bppBGRA;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetResolution(double* dpi_x, double* dpi_y) override {
    if (dpi_x == nullptr || dpi_y == nullptr) {
      return E_INVALIDARG;
    }
    *dpi_x = 96.0;
    *dpi_y = 96.0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette* /*palette*/) override {
    // The output is always 32bpp BGRA; no palette is exposed.
    return WINCODEC_ERR_PALETTEUNAVAILABLE;
  }

  HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* rect, UINT stride, UINT buffer_size, BYTE* buffer) override {
    if (nullptr == buffer) {
      return E_INVALIDARG;
    }
    WICRect area{0, 0, static_cast<INT>(geo_.width), static_cast<INT>(geo_.height)};
    if (rect != nullptr) {
      area = *rect;
    }
    if (area.Width <= 0 || area.Height <= 0) {
      return S_OK;
    }
    if (area.X < 0 || area.Y < 0 || static_cast<UINT>(area.X + area.Width) > geo_.width ||
        static_cast<UINT>(area.Y + area.Height) > geo_.height) {
      return E_INVALIDARG;
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(area.Width) * 4u;
    if (stride < row_bytes || buffer_size < row_bytes * static_cast<std::uint64_t>(area.Height)) {
      return E_INVALIDARG;
    }

    for (INT row = 0; row < area.Height; ++row) {
      const UINT dest_y = static_cast<UINT>(area.Y + row);
      const UINT src_y = geo_.top_down ? dest_y : (geo_.height - 1u - dest_y);
      const std::uint8_t* src_row = geo_.pixels + static_cast<std::size_t>(src_y) * geo_.stride;
      std::uint8_t* dst_row = buffer + static_cast<std::size_t>(row) * stride;
      for (INT col = 0; col < area.Width; ++col) {
        const UINT src_x = static_cast<UINT>(area.X + col);
        std::uint8_t b = 0;
        std::uint8_t g = 0;
        std::uint8_t r = 0;
        std::uint8_t a = 255u;
        if (geo_.indexed) {
          const std::uint32_t index = ReadIndex(src_row, src_x);
          if (index < geo_.palette_entries) {
            const std::uint8_t* entry = geo_.palette + index * 4u;
            b = entry[0];
            g = entry[1];
            r = entry[2];
          }
        } else if (geo_.bitcount == 24u) {
          const std::uint8_t* px = src_row + static_cast<std::size_t>(src_x) * 3u;
          b = px[0];
          g = px[1];
          r = px[2];
        } else if (geo_.bitcount == 16u) {
          const std::uint32_t px = ReadU16(src_row + static_cast<std::size_t>(src_x) * 2u);
          r = ExtractChannel(px, geo_.red);
          g = ExtractChannel(px, geo_.green);
          b = ExtractChannel(px, geo_.blue);
        } else {
          const std::uint32_t px = ReadU32(src_row + static_cast<std::size_t>(src_x) * 4u);
          r = ExtractChannel(px, geo_.red);
          g = ExtractChannel(px, geo_.green);
          b = ExtractChannel(px, geo_.blue);
          a = geo_.has_alpha ? ExtractChannel(px, geo_.alpha) : 255u;
        }
        std::uint8_t* dst = dst_row + static_cast<std::size_t>(col) * 4u;
        dst[0] = b;
        dst[1] = g;
        dst[2] = r;
        dst[3] = a;
      }
    }
    return S_OK;
  }

 private:
  std::uint32_t ReadIndex(const std::uint8_t* row, UINT x) const noexcept {
    if (geo_.bitcount == 8u) {
      return row[x];
    }
    if (geo_.bitcount == 4u) {
      const std::uint8_t byte = row[x / 2u];
      return ((x & 1u) == 0u) ? static_cast<std::uint32_t>(byte >> 4) : (byte & 0x0Fu);
    }
    // 1bpp
    const std::uint8_t byte = row[x / 8u];
    return static_cast<std::uint32_t>((byte >> (7u - (x & 7u))) & 1u);
  }

  DibGeometry geo_{};
  volatile LONG ref_count_ = 1;
};

// ---------------------------------------------------------------------------
// COM pointer with deterministic release on every early-return path.
// ---------------------------------------------------------------------------

template <typename T>
class ComPtr final {
 public:
  ComPtr() noexcept = default;
  ~ComPtr() noexcept { Reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  T** Put() noexcept {
    Reset();
    return &ptr_;
  }
  T* Get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  void Reset() noexcept {
    if (ptr_ != nullptr) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};

// ---------------------------------------------------------------------------
// 3. WIC PNG encode path and temp file helpers
// ---------------------------------------------------------------------------

// Initialise COM on this thread, create the WIC imaging factory and open
// `out_path` as a writable stream. `com` must outlive the factory.
Error OpenPngSink(ComApartment* com, ComPtr<IWICImagingFactory>* factory, ComPtr<IStream>* stream,
                  const wchar_t* out_path) noexcept {
  if (com->Init() == false) {
    return Error::kPngEncodeFailed;
  }
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                reinterpret_cast<void**>(factory->Put()));
  if (FAILED(hr)) {
    return Error::kPngEncodeFailed;
  }
  hr = SHCreateStreamOnFileEx(out_path, STGM_WRITE | STGM_CREATE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, stream->Put());
  if (FAILED(hr)) {
    return Error::kTempFileFailed;
  }
  return Error::kOk;
}

// Run the full PNG encode into `stream`, commit it, and report the byte count.
// `error` distinguishes an encoder failure from a stream I/O failure.
bool EncodeDibToStream(IWICImagingFactory* factory, IStream* stream, IWICBitmapSource* source, const DibGeometry& geo,
                       std::uint64_t* produced, Error* error) noexcept {
  ComPtr<IWICBitmapEncoder> encoder;
  HRESULT hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put());
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
  }
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> frame_props;
  if (SUCCEEDED(hr)) {
    hr = encoder->CreateNewFrame(frame.Put(), frame_props.Put());
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Initialize(frame_props.Get());
  }
  if (SUCCEEDED(hr)) {
    hr = frame->SetSize(geo.width, geo.height);
  }

  WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(hr)) {
    hr = frame->SetPixelFormat(&pixel_format);
  }

  ComPtr<IWICFormatConverter> converter;
  IWICBitmapSource* write_source = source;
  if (SUCCEEDED(hr) && IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA) == 0) {
    hr = factory->CreateFormatConverter(converter.Put());
    if (SUCCEEDED(hr)) {
      hr = converter->Initialize(source, pixel_format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
      write_source = converter.Get();
    }
  }
  if (SUCCEEDED(hr)) {
    hr = frame->WriteSource(write_source, nullptr);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Commit();
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Commit();
  }

  if (FAILED(hr)) {
    *error = Error::kPngEncodeFailed;
    return false;
  }
  if (FAILED(stream->Commit(STGC_DEFAULT))) {
    *error = Error::kTempFileFailed;
    return false;
  }
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) {
    *error = Error::kTempFileFailed;
    return false;
  }
  *produced = stat.cbSize.QuadPart;
  return true;
}

bool MakeTempPath(wchar_t* out, std::size_t out_chars) noexcept {
  wchar_t dir[MAX_PATH + 1] = {};
  const DWORD dir_len = GetTempPathW(MAX_PATH, dir);
  if (dir_len == 0 || dir_len > MAX_PATH) {
    return false;
  }
  // GetTempFileName creates a zero-byte file and guarantees a unique name. Both
  // encode (SHCreateStreamOnFileEx STGM_CREATE) and the copy path overwrite it.
  const UINT name_len = GetTempFileNameW(dir, L"ccc", 0, out);
  (void)out_chars;
  return name_len != 0;
}

// Deletes the temp file unless Keep() was called, so no error path leaks one.
class TempFileGuard final {
 public:
  explicit TempFileGuard(wchar_t* path) noexcept : path_(path) {}
  ~TempFileGuard() noexcept {
    if (armed_ && path_ != nullptr) {
      DeleteFileW(path_);
    }
  }
  TempFileGuard(const TempFileGuard&) = delete;
  TempFileGuard& operator=(const TempFileGuard&) = delete;
  void Keep() noexcept { armed_ = false; }

 private:
  wchar_t* path_ = nullptr;
  bool armed_ = true;
};

bool WriteFileAll(const wchar_t* path, const void* data, std::size_t len) noexcept {
  UniqueHandle file(CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
  if (file.valid() == false) {
    return false;
  }
  const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t offset = 0;
  while (offset < len) {
    const std::size_t remaining = len - offset;
    const DWORD chunk = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
    DWORD written = 0;
    if (WriteFile(file.get(), bytes + offset, chunk, &written, nullptr) == 0) {
      return false;
    }
    if (written == 0) {
      return false;
    }
    offset += written;
  }
  return true;
}

// OpenClipboard can transiently fail when another process owns the clipboard.
// Retry over a bounded window well inside the "few hundred ms" budget; this is
// a bounded operation wait, not an idle poll.
bool OpenClipboardWithRetry(HWND owner) noexcept {
  for (std::int32_t attempt = 0; attempt < 10; ++attempt) {
    if (OpenClipboard(owner) != 0) {
      return true;
    }
    Sleep(20);
  }
  return false;
}

struct ClipboardCloser final {
  ~ClipboardCloser() noexcept { CloseClipboard(); }
};

// Fill a CapturedImage from a finished temp file.
void FillCaptured(CapturedImage* image, const wchar_t* wide_path, std::uint64_t bytes, const char* source) noexcept {
  image->bytes = bytes;
  (void)CopyWide(wide_path, image->wide_path, kCapturedWideChars);
  char utf8[kCapturedPathBytes] = {};
  if (WideToUtf8(wide_path, utf8, kCapturedPathBytes)) {
    image->path_utf8.assign(osp::TruncateToCapacity, utf8);
  }
  image->source.assign(osp::TruncateToCapacity, source);
}

// Fetch one clipboard block by format. GetClipboardData can answer NULL while
// the owner is still handing the block over, so it is retried briefly. Only
// formats the clipboard really carries are ever passed in, so this can never
// pull in a synthesized block (see InspectClipboardFormats for why that
// matters).
HANDLE FetchClipboardBlock(UINT format) noexcept {
  for (std::int32_t attempt = 0; attempt < kFetchAttempts; ++attempt) {
    const HANDLE data = GetClipboardData(format);
    if (nullptr != data) {
      return data;
    }
    Sleep(kFetchRetryMs);
  }
  return nullptr;
}

// Fast path: a registered "PNG" format carries ready-to-upload bytes, so the
// block is copied to the temp file without re-encoding.
Result<CapturedImage> CaptureRegisteredPng(HANDLE data, const Config& cfg, const wchar_t* temp_path) noexcept {
  const SIZE_T size = GlobalSize(data);
  if (size == 0 || size > cfg.max_image_bytes) {
    return Result<CapturedImage>::error(size == 0 ? Error::kClipboardLockFailed : Error::kImageTooLarge);
  }
  const void* locked = GlobalLock(data);
  if (locked == nullptr) {
    return Result<CapturedImage>::error(Error::kClipboardLockFailed);
  }
  const bool wrote = WriteFileAll(temp_path, locked, size);
  GlobalUnlock(data);
  if (wrote == false) {
    return Result<CapturedImage>::error(Error::kTempFileFailed);
  }
  CapturedImage image{};
  FillCaptured(&image, temp_path, size, "PNG");
  return Result<CapturedImage>::success(image);
}

// Snapshot one DIB block the clipboard really carries. The pixels are read in
// place -- the encoder points straight at the locked HGLOBAL, so no copy of the
// image is made here. `label` names the block for the report.
Result<CapturedImage> CaptureOneDib(const Config& cfg, const wchar_t* temp_path, UINT format,
                                    const char* label) noexcept {
  const HANDLE data = FetchClipboardBlock(format);
  if (nullptr == data) {
    return Result<CapturedImage>::error(Error::kClipboardReadFailed);
  }
  const SIZE_T size = GlobalSize(data);
  const void* locked = (size == 0) ? nullptr : GlobalLock(data);
  if (nullptr == locked) {
    return Result<CapturedImage>::error(Error::kClipboardLockFailed);
  }
  Result<CapturedImage> encoded =
      EncodeDibToPngFile(static_cast<const std::uint8_t*>(locked), static_cast<std::size_t>(size), temp_path,
                         cfg.max_image_bytes);
  GlobalUnlock(data);
  if (encoded.has_value()) {
    encoded.value().source.assign(osp::TruncateToCapacity, label);
  }
  return encoded;
}

// Fallback: unpack and encode a DIB. The clipboard can carry both flavours at
// once, and a block that is listed can still refuse to hand over its bytes (the
// owner may die between the listing and the fetch), so every candidate is tried
// before a failure is reported: an unreadable CF_DIBV5 next to a perfectly good
// CF_DIB must not surface as "no image".
Result<CapturedImage> CaptureDibFallback(const Config& cfg, const wchar_t* temp_path,
                                         const ClipboardFormats& formats) noexcept {
  Error failure = Error::kNoImageInClipboard;
  if (formats.has_dibv5) {
    Result<CapturedImage> v5 = CaptureOneDib(cfg, temp_path, CF_DIBV5, "DIBV5");
    if (v5.has_value()) {
      return v5;
    }
    failure = v5.get_error();
  }
  if (formats.has_dib) {
    Result<CapturedImage> dib = CaptureOneDib(cfg, temp_path, CF_DIB, "DIB");
    if (dib.has_value()) {
      return dib;
    }
    failure = dib.get_error();
  }
  if (formats.has_bitmap) {
    // Last resort for a clipboard that publishes only CF_BITMAP: it carries no
    // DIB block, so Windows has to build one on request. That build is a full
    // copy of the pixels inside this process -- exactly what the commit ceiling
    // refuses for a large screenshot -- but without it such a clipboard is
    // unreadable, so it is tried once nothing better exists.
    Result<CapturedImage> built = CaptureOneDib(cfg, temp_path, CF_DIB, "BITMAP");
    if (built.has_value()) {
      return built;
    }
    failure = built.get_error();
  }
  return Result<CapturedImage>::error(failure);
}

}  // namespace

// ---------------------------------------------------------------------------
// 4. Public entry points
// ---------------------------------------------------------------------------

ClipboardFormats InspectClipboardFormats() noexcept {
  ClipboardFormats formats{};
  const UINT png_format = RegisterClipboardFormatW(L"PNG");
  // EnumClipboardFormats lists the blocks the owner really published, which is
  // the question the capture path must ask. IsClipboardFormatAvailable answers a
  // different one -- "could you get this format?" -- and counts formats Windows
  // would have to synthesize; asking for one of those costs a full extra copy of
  // the image inside this process, which is what a large screenshot could not
  // afford under the commit ceiling.
  UINT format = 0;
  while ((format = EnumClipboardFormats(format)) != 0u) {
    formats.has_png = formats.has_png || (format == png_format);
    formats.has_dibv5 = formats.has_dibv5 || (format == CF_DIBV5);
    formats.has_dib = formats.has_dib || (format == CF_DIB);
    formats.has_bitmap = formats.has_bitmap || (format == CF_BITMAP);
  }
  formats.has_any_image = formats.has_png || formats.has_dibv5 || formats.has_dib || formats.has_bitmap;
  return formats;
}

Result<CapturedImage> EncodeDibToPngFile(const std::uint8_t* dib, std::size_t dib_bytes, const wchar_t* out_path,
                                         std::uint32_t max_bytes) noexcept {
  DibGeometry geo{};
  if (nullptr == dib || nullptr == out_path || ParseDib(dib, dib_bytes, &geo) == false || nullptr == geo.pixels) {
    return Result<CapturedImage>::error(Error::kPngEncodeFailed);
  }

  TempFileGuard temp_guard(const_cast<wchar_t*>(out_path));

  ComApartment com{};
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IStream> stream;
  const Error sink_error = OpenPngSink(&com, &factory, &stream, out_path);
  if (sink_error != Error::kOk) {
    return Result<CapturedImage>::error(sink_error);
  }

  // Non-owning, stack-allocated zero-copy reader: the encoder pulls scanlines
  // straight out of the locked DIB. It outlives every encoder object, because
  // those live only inside EncodeDibToStream.
  DibSource source;
  source.Init(geo);

  std::uint64_t produced = 0;
  Error encode_error = Error::kOk;
  if (EncodeDibToStream(factory.Get(), stream.Get(), &source, geo, &produced, &encode_error) == false) {
    return Result<CapturedImage>::error(encode_error);
  }
  if (produced > max_bytes) {
    return Result<CapturedImage>::error(Error::kImageTooLarge);
  }

  temp_guard.Keep();
  CapturedImage image{};
  FillCaptured(&image, out_path, produced, "DIB");
  return Result<CapturedImage>::success(image);
}

Result<CapturedImage> CaptureClipboardImage(const Config& cfg) noexcept {
  ComApartment com{};
  (void)com.Init();

  if (OpenClipboardWithRetry(nullptr) == false) {
    return Result<CapturedImage>::error(Error::kClipboardOpenFailed);
  }
  ClipboardCloser closer;

  const ClipboardFormats formats = InspectClipboardFormats();
  if (formats.has_any_image == false) {
    return Result<CapturedImage>::error(Error::kNoImageInClipboard);
  }

  wchar_t temp_path[kCapturedWideChars] = {};
  if (MakeTempPath(temp_path, kCapturedWideChars) == false) {
    return Result<CapturedImage>::error(Error::kTempFileFailed);
  }
  TempFileGuard temp_guard(temp_path);

  // Fast path: a registered "PNG" format carries ready-to-upload bytes.
  if (formats.has_png) {
    const HANDLE png_data = FetchClipboardBlock(RegisterClipboardFormatW(L"PNG"));
    if (nullptr != png_data) {
      Result<CapturedImage> png = CaptureRegisteredPng(png_data, cfg, temp_path);
      if (png.has_value()) {
        temp_guard.Keep();
      }
      return png;
    }
  }

  // Fallback: unpack and encode the DIB. The DIB is read in place -- no heap
  // copy of the uncompressed pixels.
  Result<CapturedImage> encoded = CaptureDibFallback(cfg, temp_path, formats);
  if (encoded.has_value()) {
    temp_guard.Keep();
  }
  return encoded;
}

void DeleteCapturedImage(const CapturedImage& image) noexcept {
  if (image.wide_path[0] != L'\0') {
    DeleteFileW(image.wide_path);
  }
}

}  // namespace picopaste::win32

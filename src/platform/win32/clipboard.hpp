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
 * @file clipboard.hpp
 * @brief Win32 clipboard capture to a single temporary file.
 *
 * The clipboard is opened, snapshotted into one temp file, and released before
 * this module returns. Nothing downstream ever touches a clipboard HGLOBAL: a
 * multi-second SFTP upload holding the clipboard lock (and depending on an
 * HGLOBAL that the owner may replace underneath it) was the rejected design.
 *
 * Memory discipline (the hard requirement in the design doc, section 5): the
 * uncompressed DIB -- a 4K screenshot is about 33 MB -- is NEVER copied into
 * our heap. When only a DIB is on the clipboard, a small IWICBitmapSource wraps
 * the locked HGLOBAL directly and the PNG encoder reads scanlines straight out
 * of it, so our own allocation for the image is a bounded encoder buffer.
 */
#pragma once

//
// NOTE: this header intentionally includes <windows.h>; it is a Windows-layer
// header and is only ever compiled into the WIN32 client target.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// See win32_util.hpp: osp/platform.hpp must precede <windows.h> so newosp does
// not mistake windows.h's RT_VERSION macro for the RT-Thread marker.
#include "osp/platform.hpp"
#include "osp/vocabulary.hpp"
#include "picopaste/config.hpp"
#include "picopaste/error.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace picopaste::win32 {

// Longest temp path we will build. GetTempPathW is bounded by MAX_PATH in the
// common case; 512 leaves headroom without a heap string.
inline constexpr std::uint32_t kCapturedPathBytes = 512;
inline constexpr std::uint32_t kCapturedWideChars = 512;

// The temp file a capture produced. `path_utf8` is what the SFTP uploader
// opens; `wide_path` is what the Win32 delete uses.
struct CapturedImage {
  osp::FixedString<kCapturedPathBytes> path_utf8{};
  wchar_t wide_path[kCapturedWideChars]{};
  std::uint64_t bytes = 0;
  // "PNG" (registered format fast path), "DIBV5" or "DIB" (WIC encode path).
  osp::FixedString<16> source{};
};

// Which image-bearing clipboard formats are present right now. Used by the
// selftest report so "no image" is a visible, specific answer.
struct ClipboardFormats {
  bool has_png = false;
  bool has_dibv5 = false;
  bool has_dib = false;
  bool has_any_image = false;
};

/** @brief Pure format probe. Does not read pixels and does not create a temp file. */
ClipboardFormats InspectClipboardFormats() noexcept;

/**
 * @brief Capture the clipboard image to one temp file.
 *
 * The clipboard is closed before return.
 * @return kNoImageInClipboard when the clipboard holds no image; kImageTooLarge
 * when the encoded image would exceed cfg.max_image_bytes; kPngEncodeFailed /
 * kTempFileFailed / kClipboardOpenFailed on hard failures.
 */
Result<CapturedImage> CaptureClipboardImage(const Config& cfg) noexcept;

/**
 * @brief Encode a raw DIB (BITMAPINFOHEADER, BITMAPV4HEADER or BITMAPV5HEADER
 * followed by palette and pixels) to `out_path` as PNG.
 *
 * Exposed so selftest can exercise the encoder with a synthetic DIB without a
 * real clipboard.
 * @param dib must stay valid for the duration of the call; it is read in place,
 * never copied.
 */
Result<CapturedImage> EncodeDibToPngFile(const std::uint8_t* dib, std::size_t dib_bytes, const wchar_t* out_path,
                                         std::uint32_t max_bytes) noexcept;

/**
 * @brief Delete the temp file behind `image`.
 *
 * Safe to call on a default-constructed value and safe to call twice.
 */
void DeleteCapturedImage(const CapturedImage& image) noexcept;

}  // namespace picopaste::win32

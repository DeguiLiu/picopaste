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
 * @file win32_util.hpp
 * @brief Windows platform layer shared helpers.
 *
 * Private to src/platform/win32. It exists so every module in this directory
 * pairs CloseHandle on its early-return paths the same way, and so the narrow /
 * wide conversion used at the shell boundary lives in exactly one place.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Must precede <windows.h>. windows.h defines RT_VERSION (a resource type),
// which newosp's platform.hpp otherwise reads as the RT-Thread RTOS marker and
// tries to include <rtthread.h>. Pulling platform.hpp first evaluates that
// detection before the macro exists.
#include "osp/platform.hpp"
#include "picopaste/error.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include <objbase.h>

namespace picopaste::win32 {

// Owns a kernel HANDLE (file, pipe, process, thread, mutex, job, event, ...).
// Move-only; reset() nulls the handle before closing so double-reset is safe.
class UniqueHandle final {
 public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueHandle() noexcept { reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      reset();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  HANDLE get() const noexcept { return handle_; }
  explicit operator bool() const noexcept { return handle_ != nullptr; }

  // True when the handle is not NULL and not INVALID_HANDLE_VALUE.
  bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

  HANDLE release() noexcept {
    HANDLE released = handle_;
    handle_ = nullptr;
    return released;
  }

  void reset(HANDLE handle = nullptr) noexcept {
    if (handle_ != nullptr) {
      CloseHandle(handle_);
    }
    handle_ = handle;
  }

 private:
  HANDLE handle_ = nullptr;
};

// Per-thread COM apartment scope. WIC works in either apartment; the OLE
// clipboard helpers want a single-threaded apartment. Create one of these on
// every thread that touches WIC or the clipboard, before first use.
//
// CoInitializeEx returning S_FALSE means "already initialized by this thread";
// the matching CoUninitialize is still required, so owned_ is set in that case
// too. RPC_E_CHANGED_MODE means COM is already live with a different apartment
// model: usable, but we did not add a reference and must not remove one.
class ComApartment final {
 public:
  ComApartment() noexcept = default;
  ~ComApartment() noexcept {
    if (owned_) {
      CoUninitialize();
    }
  }
  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  // Returns true when COM is usable on this thread.
  bool Init() noexcept {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) {
      owned_ = true;
      return true;
    }
    if (hr == RPC_E_CHANGED_MODE) {
      owned_ = false;
      return true;
    }
    owned_ = false;
    return false;
  }

 private:
  bool owned_ = false;
};

// UTF-8 -> UTF-16 into a fixed buffer (NUL terminated). Returns false when the
// result would not fit, so callers report instead of truncating silently.
inline bool Utf8ToWide(const char* in, wchar_t* out, std::size_t out_chars) noexcept {
  if (nullptr == in || nullptr == out || 0 == out_chars) {
    return false;
  }
  std::int32_t written = MultiByteToWideChar(CP_UTF8, 0, in, -1, out, static_cast<int>(out_chars));
  return written > 0;
}

// UTF-16 -> UTF-8 into a fixed buffer (NUL terminated). Returns false when the
// result would not fit.
inline bool WideToUtf8(const wchar_t* in, char* out, std::size_t out_bytes) noexcept {
  if (nullptr == in || nullptr == out || 0 == out_bytes) {
    return false;
  }
  std::int32_t written = WideCharToMultiByte(CP_UTF8, 0, in, -1, out, static_cast<int>(out_bytes), nullptr, nullptr);
  return written > 0;
}

// Copy a NUL-terminated source into a fixed wide buffer. False when too long.
inline bool CopyWide(const wchar_t* src, wchar_t* dst, std::size_t dst_chars) noexcept {
  if (nullptr == src || nullptr == dst || 0 == dst_chars) {
    return false;
  }
  std::size_t i = 0;
  while (i + 1 < dst_chars && src[i] != L'\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = L'\0';
  return src[i] == L'\0';
}

}  // namespace picopaste::win32

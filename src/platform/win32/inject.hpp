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
 * @file inject.hpp
 * @brief Paste delivery: focus guard + SendInput Ctrl+Shift+V.
 *
 * The remote path is put on the clipboard with the vendored clip library (its
 * raw SetClipboardData path leaves a system-owned HGLOBAL, so the text outlives
 * this process without an OLE flush). The keystroke itself is SendInput. The
 * modifier-release pre-pass is a separate SendInput call with its own
 * submitted-count check, and the 6-event Ctrl+Shift+V chord is checked against
 * its own submitted count, so a refused insert is reported, never mistaken for
 * success.
 */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// See win32_util.hpp: osp/platform.hpp must precede <windows.h> so newosp does
// not mistake windows.h's RT_VERSION macro for the RT-Thread marker.
#include "osp/platform.hpp"
#include "picopaste/error.hpp"

#include <windows.h>

#include <cstdint>

namespace picopaste::win32 {

/**
 * @brief Current foreground window, or nullptr when no window has focus.
 *
 * The baseline must be captured before anything else touches the clipboard.
 */
HWND CaptureForegroundWindow() noexcept;

struct PasteRequest {
  // Baseline captured before the upload started. Passed in so the guard sees
  // the window the hotkey was aimed at, not whatever is focused now.
  HWND target_window = nullptr;
  // UTF-8 remote path to place on the clipboard.
  const char* remote_path_utf8 = nullptr;
  // Milliseconds to wait before re-checking focus and sending the chord.
  std::uint32_t delay_ms = 150;
  // Optional PNG temp file to restore as the clipboard image afterwards.
  const wchar_t* restore_png_path = nullptr;
};

// Outcome of one SendInput batch.
struct SendChordResult {
  bool ok = false;
  std::uint32_t inserted = 0;
  std::uint32_t requested = 0;
  DWORD last_error = 0;
};

/**
 * @brief Release any physically held Alt/Win key, then send the 6-event
 * Ctrl+Shift+V chord.
 *
 * The release pre-pass and the chord are each count-checked by their own
 * SendInput call.
 */
SendChordResult SendPasteChord() noexcept;

/**
 * @brief Set the clipboard text, wait, re-check focus, send the chord, and
 * optionally restore the image.
 *
 * kFocusChanged withholds the keystroke; kSendInputRejected reports a short
 * insert.
 */
Status DeliverPaste(const PasteRequest& request) noexcept;

/** @brief Put a PNG file on the clipboard under the registered "PNG" format. */
Status SetClipboardPngFile(const wchar_t* path) noexcept;

}  // namespace picopaste::win32

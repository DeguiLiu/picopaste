// picopaste -- paste delivery: focus guard + SendInput Ctrl+Shift+V.
//
// The remote path is put on the clipboard with the vendored clip library (its
// raw SetClipboardData path leaves a system-owned HGLOBAL, so the text outlives
// this process without an OLE flush). The keystroke itself is SendInput, and
// its return value is checked against the number of events submitted --
// including any modifier-release events added first -- so a refused insert is
// reported, never mistaken for success.
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

#include <windows.h>

#include <cstdint>

#include "picopaste/error.hpp"

namespace picopaste::win32 {

// Current foreground window, or nullptr when no window has focus. The baseline
// must be captured before anything else touches the clipboard.
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
  int inserted = 0;
  int requested = 0;
  DWORD last_error = 0;
};

// Release any physically held Alt/Win key, then send Ctrl+Shift+V, verifying
// the accepted event count against the submitted count.
SendChordResult SendPasteChord() noexcept;

// Set the clipboard text, wait, re-check focus, send the chord, and optionally
// restore the image. kFocusChanged withholds the keystroke; kSendInputRejected
// reports a short insert.
Status DeliverPaste(const PasteRequest& request) noexcept;

// Put a PNG file on the clipboard under the registered "PNG" format.
Status SetClipboardPngFile(const wchar_t* path) noexcept;

}  // namespace picopaste::win32

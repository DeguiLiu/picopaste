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
 * @file hotkey.hpp
 * @brief Global hotkey parsing and acquisition.
 *
 * Registration failure must be reported immediately with the exact
 * combination; the previous implementation printed "started" and exited 0,
 * leaving the user to discover the dead hotkey later (design section 9).
 *
 * RegisterHotKey is the first choice. When it fails because another process
 * already owns the chord (ERROR_HOTKEY_ALREADY_REGISTERED) the tool falls back
 * to a WH_KEYBOARD_LL hook, which sees the keystroke before the system
 * dispatches it and can therefore make the other registration dead. Which path
 * is live is never hidden: HotkeyBackend is reported to the self-check, the
 * startup log and the tray tooltip.
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
#include "osp/vocabulary.hpp"
#include "picopaste/error.hpp"

#include <windows.h>

#include <cstdint>

namespace picopaste::win32 {

// WM_HOTKEY, posted to the registering thread's queue (or its window).
inline constexpr UINT kWmHotkey = 0x0312;

struct HotkeyBinding {
  UINT modifiers = 0;              // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
  UINT vk = 0;                     // virtual-key code
  osp::FixedString<64> display{};  // canonical, e.g. "ctrl+alt+v"
};

/**
 * @brief Parse "alt+shift+v".
 *
 * Requires at least one modifier and one key. Rejects duplicate tokens, an
 * unknown key, and any key that collides with the simulated paste chord
 * (ctrl+shift+v) or the system paste (ctrl+v). A null or empty string yields
 * the documented default alt+shift+v.
 */
Result<HotkeyBinding> ParseHotkey(const char* text) noexcept;

/**
 * @brief Register the chord with MOD_NOREPEAT added.
 *
 * @param hwnd may be nullptr (WM_HOTKEY is then posted to the calling thread's
 * queue).
 * @param last_error receives the failing RegisterHotKey error (1409 when the
 * fallback engaged) or the hook-install error when the fallback itself failed
 * (may be null).
 * @return Success when either RegisterHotKey owns the chord or a low-level hook
 * was installed instead. RegisterHotKey is tried first; the fallback engages
 * when, and only when, it fails with ERROR_HOTKEY_ALREADY_REGISTERED.
 */
Status RegisterHotkey(HWND hwnd, INT id, const HotkeyBinding& binding, DWORD* last_error) noexcept;

void UnregisterHotkey(HWND hwnd, INT id) noexcept;

// Which mechanism currently owns the chord. RegisterHotKey is always tried
// first; the hook is installed only because another process already owned the
// combination. Exposed so the self-check, the tray tooltip and the startup log
// name the live path instead of implying registration always succeeded.
enum class HotkeyBackend : std::uint8_t {
  kNone = 0,        // no path is active
  kRegistered = 1,  // RegisterHotKey owns the chord
  kHook = 2,        // WH_KEYBOARD_LL filters the chord (it was taken)
};

/** @brief Stable, human-readable name for `backend`. Never null. */
const char* HotkeyBackendName(HotkeyBackend backend) noexcept;

/**
 * @brief Backend currently owned by the free-function API above.
 *
 * kNone once released. The self-check reads this to print which path its
 * register-hotkey line took.
 */
HotkeyBackend ActiveHotkeyBackend() noexcept;

// Owns exactly one active hotkey path and releases it on destruction, so every
// exit path unregisters or unhooks. Install tries RegisterHotKey first; a
// failure with ERROR_HOTKEY_ALREADY_REGISTERED installs a WH_KEYBOARD_LL hook
// on the calling thread (which must pump messages). Any other failure is
// returned unchanged -- the fallback never swallows a different error. On the
// hook path the callback also posts kWmHotkey to this thread, so the message
// loop's existing WM_HOTKEY handling triggers an upload.
class HotkeyRegistration final {
 public:
  HotkeyRegistration() noexcept = default;
  ~HotkeyRegistration() noexcept { Uninstall(); }
  HotkeyRegistration(const HotkeyRegistration&) = delete;
  HotkeyRegistration& operator=(const HotkeyRegistration&) = delete;

  /**
   * @brief Acquire the chord, one-shot per instance.
   * @param hwnd/id used only by the RegisterHotKey path; the hook path posts to
   * the calling thread's message queue instead.
   */
  Status Install(HWND hwnd, INT id, const HotkeyBinding& binding) noexcept;
  void Uninstall() noexcept;

  bool active() const noexcept { return backend_ != HotkeyBackend::kNone; }
  HotkeyBackend backend() const noexcept { return backend_; }
  // GetLastError() from the failed RegisterHotKey, or from SetWindowsHookExW
  // when the fallback itself failed. 0 when the last Install succeeded.
  DWORD last_error() const noexcept { return last_error_; }

 private:
  HWND hwnd_ = nullptr;
  INT id_ = 0;
  HHOOK hook_ = nullptr;  // non-null only on the kHook path
  HotkeyBackend backend_ = HotkeyBackend::kNone;
  DWORD last_error_ = 0;
};

}  // namespace picopaste::win32

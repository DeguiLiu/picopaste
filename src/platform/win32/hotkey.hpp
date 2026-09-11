// picopaste -- global hotkey parsing and RegisterHotKey.
//
// Registration failure must be reported immediately with the exact
// combination; the previous implementation printed "started" and exited 0,
// leaving the user to discover the dead hotkey later (design section 9).
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
#include "osp/vocabulary.hpp"

namespace picopaste::win32 {

// WM_HOTKEY, posted to the registering thread's queue (or its window).
inline constexpr UINT kWmHotkey = 0x0312;

struct HotkeyBinding {
  UINT modifiers = 0;  // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
  UINT vk = 0;         // virtual-key code
  osp::FixedString<64> display{};  // canonical, e.g. "ctrl+alt+v"
};

// Parse "alt+shift+v". Requires at least one modifier and one key. Rejects
// duplicate tokens, an unknown key, and any key that collides with the
// simulated paste chord (ctrl+shift+v) or the system paste (ctrl+v). A null or
// empty string yields the documented default alt+shift+v.
Result<HotkeyBinding> ParseHotkey(const char* text) noexcept;

// Register with MOD_NOREPEAT added. `hwnd` may be nullptr (WM_HOTKEY is then
// posted to the calling thread's queue). On failure returns
// kHotkeyRegisterFailed and writes GetLastError() to `last_error` when non-null.
Status RegisterHotkey(HWND hwnd, int id, const HotkeyBinding& binding,
                      DWORD* last_error) noexcept;

void UnregisterHotkey(HWND hwnd, int id) noexcept;

}  // namespace picopaste::win32

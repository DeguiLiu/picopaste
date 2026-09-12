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
 * @file hotkey.cpp
 * @brief Global hotkey implementation.
 */
#include "hotkey.hpp"

#include <cstring>

namespace picopaste::win32 {
namespace {

constexpr UINT kModAlt = 0x0001;
constexpr UINT kModControl = 0x0002;
constexpr UINT kModShift = 0x0004;
constexpr UINT kModWin = 0x0008;
constexpr UINT kModNoRepeat = 0x4000;
constexpr UINT kVkControl = 0x11;
constexpr UINT kVkV = 0x56;

// Read the live modifier state. Called from the hook callback, so it consults
// GetAsyncKeyState only -- no allocation, no blocking.
UINT ModifiersDown() noexcept {
  UINT modifiers = 0;
  if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
    modifiers |= kModControl;
  }
  if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
    modifiers |= kModAlt;
  }
  if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
    modifiers |= kModShift;
  }
  if (((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0) {
    modifiers |= kModWin;
  }
  return modifiers;
}

// The one chord a low-level hook is filtering. A WH_KEYBOARD_LL callback runs
// on the thread that installed the hook (the message-pumping thread), never
// concurrently, so plain statics keep the callback to a few loads and compares.
struct ActiveChord {
  UINT vk = 0;
  UINT modifiers = 0;
  WPARAM id = 0;
  bool armed = false;
  bool key_down = false;  // mirrors MOD_NOREPEAT: re-post only on a fresh press
};
ActiveChord g_active_chord{};

// The free-function API's owner. The self-check registers and releases exactly
// one chord through it; HotkeyRegistration exists for callers whose lifetime is
// a scope rather than the process.
HotkeyRegistration g_free_registration;

// Installed only when another process already owns the chord. On an exact match
// it swallows the key and posts kWmHotkey so the existing message loop runs the
// upload. It never logs, buffers or stores what was typed: the only state it
// touches is the configured chord. Synthetic (injected) input is passed
// through untouched, which is what keeps picopaste's own ctrl+shift+v paste
// from re-entering here.
LRESULT CALLBACK ChordHookProc(int code, WPARAM wparam, LPARAM lparam) {
  if ((code != HC_ACTION) || (g_active_chord.armed == false)) {
    return CallNextHookEx(nullptr, code, wparam, lparam);
  }
  const KBDLLHOOKSTRUCT* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
  if ((key == nullptr) || (static_cast<UINT>(key->vkCode) != g_active_chord.vk)) {
    return CallNextHookEx(nullptr, code, wparam, lparam);
  }
  // Alt is held for the default chord, so the trigger key arrives as a *SYS*
  // message; both forms must be recognised.
  const bool is_down = (wparam == static_cast<WPARAM>(WM_KEYDOWN)) || (wparam == static_cast<WPARAM>(WM_SYSKEYDOWN));
  const bool is_up = (wparam == static_cast<WPARAM>(WM_KEYUP)) || (wparam == static_cast<WPARAM>(WM_SYSKEYUP));
  if (is_up) {
    // Release the repeat latch even if a modifier was let go first.
    g_active_chord.key_down = false;
  }
  if (((key->flags & LLKHF_INJECTED) != 0) || (ModifiersDown() != g_active_chord.modifiers)) {
    return CallNextHookEx(nullptr, code, wparam, lparam);
  }
  if (is_down) {
    if (g_active_chord.key_down == false) {
      g_active_chord.key_down = true;
      (void)PostThreadMessageW(GetCurrentThreadId(), kWmHotkey, g_active_chord.id, 0);
    }
    return 1;  // swallow, including auto-repeat, so the other owner is dead
  }
  if (is_up) {
    return 1;
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

bool EqualsIgnoreCase(const char* a, const char* b) noexcept {
  while (*a != '\0' && *b != '\0') {
    char ca = *a;
    char cb = *b;
    if (ca >= 'A' && ca <= 'Z') {
      ca = static_cast<char>(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = static_cast<char>(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

// Parse an F-key token ("f1".."f24"). Returns 0 when it is not one.
UINT ParseFunctionKey(const char* token) noexcept {
  if ((token[0] != 'f' && token[0] != 'F') || token[1] == '\0') {
    return 0;
  }
  std::int32_t number = 0;
  for (std::int32_t i = 1; token[i] != '\0'; ++i) {
    if (token[i] < '0' || token[i] > '9') {
      return 0;
    }
    number = number * 10 + (token[i] - '0');
    if (number > 24) {
      return 0;
    }
  }
  if (number < 1) {
    return 0;
  }
  return static_cast<UINT>(0x70 + number - 1);
}

// Append a token to a fixed output buffer; returns false on overflow.
bool Append(char* out, std::size_t cap, std::size_t* len, const char* text) noexcept {
  if (*len > 0) {
    if (*len + 1 >= cap) {
      return false;
    }
    out[(*len)++] = '+';
  }
  for (std::size_t i = 0; text[i] != '\0'; ++i) {
    if (*len + 1 >= cap) {
      return false;
    }
    out[(*len)++] = text[i];
  }
  out[*len] = '\0';
  return true;
}

// Copy the next '+'-separated token out of `cursor` into `token` (NUL
// terminated) and advance `cursor` past the separator. Returns false on an
// empty token or one that does not fit.
bool NextHotkeyToken(const char** cursor, char* token, std::size_t token_cap, std::size_t* token_len) noexcept {
  const char* start = *cursor;
  while (**cursor != '\0' && **cursor != '+') {
    ++*cursor;
  }
  *token_len = static_cast<std::size_t>(*cursor - start);
  if (**cursor == '+') {
    ++*cursor;
  }
  if (*token_len == 0 || *token_len >= token_cap) {
    return false;
  }
  std::memcpy(token, start, *token_len);
  token[*token_len] = '\0';
  return true;
}

enum class TokenKind : std::uint8_t { kModifier, kKey, kInvalid };

// Recognise one modifier token and OR its bit into `modifiers`. `seen` holds
// the bits already supplied, so a repeated modifier is rejected. A token that
// is not a modifier at all is reported as kKey for the caller to resolve.
TokenKind ApplyModifier(const char* token, UINT* modifiers, std::uint8_t* seen) noexcept {
  UINT bit = 0;
  if (EqualsIgnoreCase(token, "alt")) {
    bit = kModAlt;
  } else if (EqualsIgnoreCase(token, "ctrl") || EqualsIgnoreCase(token, "control")) {
    bit = kModControl;
  } else if (EqualsIgnoreCase(token, "shift")) {
    bit = kModShift;
  } else if (EqualsIgnoreCase(token, "win") || EqualsIgnoreCase(token, "windows") || EqualsIgnoreCase(token, "meta")) {
    bit = kModWin;
  } else {
    return TokenKind::kKey;
  }
  if ((*seen & static_cast<std::uint8_t>(bit)) != 0) {
    return TokenKind::kInvalid;
  }
  *seen |= static_cast<std::uint8_t>(bit);
  *modifiers |= bit;
  return TokenKind::kModifier;
}

// Resolve a non-modifier token to a virtual key. False for an unknown key.
bool ResolveHotkeyKey(const char* token, UINT* vk) noexcept {
  if (token[1] == '\0') {
    const char ch = token[0];
    if (ch >= 'a' && ch <= 'z') {
      *vk = static_cast<UINT>(ch - 'a' + 'A');
    } else if (ch >= '0' && ch <= '9') {
      *vk = static_cast<UINT>(ch);
    } else {
      return false;
    }
    return true;
  }
  const UINT function_key = ParseFunctionKey(token);
  if (function_key != 0) {
    *vk = function_key;
    return true;
  }
  const struct NamedKey {
    const char* name;
    UINT vk;
  } kNamed[] = {
      {"insert", 0x2D},
      {"delete", 0x2E},
  };
  for (const NamedKey& key : kNamed) {
    if (EqualsIgnoreCase(token, key.name)) {
      *vk = key.vk;
      return true;
    }
  }
  return false;
}

// Build the canonical display string ("ctrl+alt+v"). False on buffer overflow.
bool BuildHotkeyDisplay(UINT modifiers, const char* key_token, osp::FixedString<64>* out) noexcept {
  char display[64] = {};
  std::size_t len = 0;
  const struct Part {
    UINT bit;
    const char* name;
  } kParts[] = {
      {kModControl, "ctrl"},
      {kModAlt, "alt"},
      {kModShift, "shift"},
      {kModWin, "win"},
  };
  for (const Part& part : kParts) {
    if ((modifiers & part.bit) != 0 && Append(display, sizeof(display), &len, part.name) == false) {
      return false;
    }
  }
  if (Append(display, sizeof(display), &len, key_token) == false) {
    return false;
  }
  out->assign(osp::TruncateToCapacity, display);
  return true;
}

// Consume the whole '+'-separated list into modifiers, the virtual key, and the
// canonical (lowercased) key token. False on any malformed token.
bool ParseHotkeyTokens(const char* value, UINT* modifiers, UINT* vk, const char** key_token,
                       char* key_token_buf) noexcept {
  std::uint8_t seen = 0;
  const char* cursor = value;
  while (*cursor != '\0') {
    char token[16] = {};
    std::size_t token_len = 0;
    if (NextHotkeyToken(&cursor, token, sizeof(token), &token_len) == false) {
      return false;
    }
    const TokenKind kind = ApplyModifier(token, modifiers, &seen);
    if (kind == TokenKind::kModifier) {
      continue;
    }
    if (kind == TokenKind::kInvalid || *key_token != nullptr) {
      return false;
    }
    // Copy out of the loop-local buffer; the pointer must stay valid until the
    // canonical display is built after the loop.
    std::memcpy(key_token_buf, token, token_len + 1);
    *key_token = key_token_buf;
    // Lowercase the token for resolution and for the display. The display keeps
    // whatever case the configuration used, because key_token_buf was filled
    // from the original token above -- `CTRL+SHIFT+F9` displays as
    // `ctrl+shift+F9`. The lowercasing below therefore only feeds
    // ResolveHotkeyKey, which matches on lowercase letters.
    for (char* p = token; *p != '\0'; ++p) {
      if (*p >= 'A' && *p <= 'Z') {
        *p = static_cast<char>(*p - 'A' + 'a');
      }
    }
    // Resolve now; store the virtual key.
    if (ResolveHotkeyKey(token, vk) == false) {
      return false;
    }
  }
  return true;
}

}  // namespace

Result<HotkeyBinding> ParseHotkey(const char* text) noexcept {
  const char* value = (text != nullptr && text[0] != '\0') ? text : "alt+shift+v";

  UINT modifiers = 0;
  UINT vk = 0;
  char key_token_buf[16] = {};
  const char* key_token = nullptr;
  if (ParseHotkeyTokens(value, &modifiers, &vk, &key_token, key_token_buf) == false) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }

  if (modifiers == 0 || vk == 0) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  // The simulated paste is ctrl+shift+v; the system paste is ctrl+v. A global
  // hotkey on either would be re-caught by our own chord and the paste would
  // never reach the terminal.
  if (vk == kVkV && (modifiers == kModControl || modifiers == (kModControl | kModShift))) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }

  HotkeyBinding binding{};
  binding.modifiers = modifiers;
  binding.vk = vk;
  if (BuildHotkeyDisplay(modifiers, key_token, &binding.display) == false) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  return Result<HotkeyBinding>::success(binding);
}

Status HotkeyRegistration::Install(HWND hwnd, INT id, const HotkeyBinding& binding) noexcept {
  if ((backend_ != HotkeyBackend::kNone) || g_active_chord.armed) {
    return Status::error(Error::kHotkeyRegisterFailed);  // one owner per process
  }
  last_error_ = 0;
  // First choice: the cheap, process-local registration.
  if (RegisterHotKey(hwnd, id, binding.modifiers | kModNoRepeat, binding.vk) != 0) {
    hwnd_ = hwnd;
    id_ = id;
    backend_ = HotkeyBackend::kRegistered;
    return Status::success();
  }
  const DWORD os_error = GetLastError();
  last_error_ = os_error;
  // Only a chord already owned by another process justifies a global hook.
  // Every other failure is a real error and stays a hard failure.
  if (os_error != static_cast<DWORD>(ERROR_HOTKEY_ALREADY_REGISTERED)) {
    return Status::error(Error::kHotkeyRegisterFailed);
  }

  // The chord is taken. RegisterHotKey has no API to reclaim it, so fall back
  // to WH_KEYBOARD_LL: the callback sees the keystroke before the system
  // dispatches it, and swallowing it makes the other registration dead.
  //
  // HAZARD: Windows silently removes a low-level hook whose callback takes
  // longer than LowLevelHooksTimeout (300 ms by default). That is the cost of
  // this path, so ChordHookProc only compares and posts -- no logging, no
  // blocking, no allocation.
  g_active_chord.vk = binding.vk;
  g_active_chord.modifiers = binding.modifiers;
  g_active_chord.id = static_cast<WPARAM>(id);
  g_active_chord.key_down = false;
  g_active_chord.armed = true;
  const HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, &ChordHookProc, GetModuleHandleW(nullptr), 0);
  if (hook == nullptr) {
    g_active_chord.armed = false;
    last_error_ = GetLastError();
    return Status::error(Error::kHotkeyRegisterFailed);
  }
  hook_ = hook;
  id_ = id;
  backend_ = HotkeyBackend::kHook;
  return Status::success();
}

void HotkeyRegistration::Uninstall() noexcept {
  if (backend_ == HotkeyBackend::kRegistered) {
    (void)UnregisterHotKey(hwnd_, id_);
  } else if (backend_ == HotkeyBackend::kHook) {
    // Disarm before unhooking so a spurious callback cannot act on stale state.
    g_active_chord.armed = false;
    (void)UnhookWindowsHookEx(hook_);
  }
  hwnd_ = nullptr;
  id_ = 0;
  hook_ = nullptr;
  backend_ = HotkeyBackend::kNone;
}

const char* HotkeyBackendName(HotkeyBackend backend) noexcept {
  switch (backend) {
    case HotkeyBackend::kRegistered:
      return "RegisterHotKey";
    case HotkeyBackend::kHook:
      return "LL-hook (chord was taken)";
    case HotkeyBackend::kNone:
    default:
      return "none";
  }
}

HotkeyBackend ActiveHotkeyBackend() noexcept {
  return g_free_registration.backend();
}

Status RegisterHotkey(HWND hwnd, INT id, const HotkeyBinding& binding, DWORD* last_error) noexcept {
  const Status installed = g_free_registration.Install(hwnd, id, binding);
  if (last_error != nullptr) {
    *last_error = g_free_registration.last_error();
  }
  return installed;
}

void UnregisterHotkey(HWND hwnd, INT id) noexcept {
  (void)hwnd;
  (void)id;
  g_free_registration.Uninstall();
}

}  // namespace picopaste::win32

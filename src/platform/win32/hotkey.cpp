// picopaste -- global hotkey implementation.
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
  int number = 0;
  for (int i = 1; token[i] != '\0'; ++i) {
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

}  // namespace

Result<HotkeyBinding> ParseHotkey(const char* text) noexcept {
  const char* value = (text != nullptr && text[0] != '\0') ? text : "alt+shift+v";

  UINT modifiers = 0;
  UINT vk = 0;
  char key_token_buf[16] = {};
  const char* key_token = nullptr;
  bool seen_alt = false;
  bool seen_ctrl = false;
  bool seen_shift = false;
  bool seen_win = false;

  const char* cursor = value;
  while (*cursor != '\0') {
    const char* start = cursor;
    while (*cursor != '\0' && *cursor != '+') {
      ++cursor;
    }
    std::size_t token_len = static_cast<std::size_t>(cursor - start);
    if (*cursor == '+') {
      ++cursor;
    }
    if (token_len == 0) {
      return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
    }
    char token[16] = {};
    if (token_len >= sizeof(token)) {
      return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
    }
    std::memcpy(token, start, token_len);

    if (EqualsIgnoreCase(token, "alt")) {
      if (seen_alt) {
        return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
      }
      seen_alt = true;
      modifiers |= kModAlt;
    } else if (EqualsIgnoreCase(token, "ctrl") || EqualsIgnoreCase(token, "control")) {
      if (seen_ctrl) {
        return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
      }
      seen_ctrl = true;
      modifiers |= kModControl;
    } else if (EqualsIgnoreCase(token, "shift")) {
      if (seen_shift) {
        return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
      }
      seen_shift = true;
      modifiers |= kModShift;
    } else if (EqualsIgnoreCase(token, "win") || EqualsIgnoreCase(token, "windows") ||
               EqualsIgnoreCase(token, "meta")) {
      if (seen_win) {
        return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
      }
      seen_win = true;
      modifiers |= kModWin;
    } else {
      if (key_token != nullptr) {
        return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
      }
      // Copy out of the loop-local buffer; the pointer must stay valid until the
      // canonical display is built after the loop.
      std::memcpy(key_token_buf, token, token_len + 1);
      key_token = key_token_buf;
      // Keep the (lowercased) key token in the canonical display.
      for (char* p = token; *p != '\0'; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
          *p = static_cast<char>(*p - 'A' + 'a');
        }
      }
      // Resolve now; store the virtual key.
      if (token_len == 1) {
        const char ch = token[0];
        if (ch >= 'a' && ch <= 'z') {
          vk = static_cast<UINT>(ch - 'a' + 'A');
        } else if (ch >= '0' && ch <= '9') {
          vk = static_cast<UINT>(ch);
        } else {
          return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
        }
      } else {
        const UINT function_key = ParseFunctionKey(token);
        if (function_key != 0) {
          vk = function_key;
        } else if (EqualsIgnoreCase(token, "insert")) {
          vk = 0x2D;
        } else if (EqualsIgnoreCase(token, "delete")) {
          vk = 0x2E;
        } else {
          return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
        }
      }
    }
  }

  if (modifiers == 0 || vk == 0) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  // The simulated paste is ctrl+shift+v; the system paste is ctrl+v. A global
  // hotkey on either would be re-caught by our own chord and the paste would
  // never reach the terminal.
  if (vk == kVkV && modifiers == (kModControl | kModShift)) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  if (vk == kVkV && modifiers == kModControl) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }

  HotkeyBinding binding{};
  binding.modifiers = modifiers;
  binding.vk = vk;
  char display[64] = {};
  std::size_t len = 0;
  display[0] = '\0';
  if ((modifiers & kModControl) != 0 && Append(display, sizeof(display), &len, "ctrl") == false) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  if ((modifiers & kModAlt) != 0 && Append(display, sizeof(display), &len, "alt") == false) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  if ((modifiers & kModShift) != 0 && Append(display, sizeof(display), &len, "shift") == false) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  if ((modifiers & kModWin) != 0 && Append(display, sizeof(display), &len, "win") == false) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  if (Append(display, sizeof(display), &len, key_token) == false) {
    return Result<HotkeyBinding>::error(Error::kHotkeyRegisterFailed);
  }
  binding.display.assign(osp::TruncateToCapacity, display);
  return Result<HotkeyBinding>::success(binding);
}

Status RegisterHotkey(HWND hwnd, int id, const HotkeyBinding& binding,
                      DWORD* last_error) noexcept {
  if (RegisterHotKey(hwnd, id, binding.modifiers | kModNoRepeat, binding.vk) == 0) {
    if (last_error != nullptr) {
      *last_error = GetLastError();
    }
    return Status::error(Error::kHotkeyRegisterFailed);
  }
  return Status::success();
}

void UnregisterHotkey(HWND hwnd, int id) noexcept {
  (void)UnregisterHotKey(hwnd, id);
}

}  // namespace picopaste::win32

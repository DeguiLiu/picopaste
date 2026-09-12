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
 * @file inject.cpp
 * @brief Paste delivery implementation.
 */
#include "inject.hpp"

#include "clip.h"
#include "clip_win_hglobal.h"
#include "win32_util.hpp"

#include <string>

#if defined(_WIN64) || defined(__x86_64__)
// INPUT is 40 bytes on x64: type (4) + union padding (4) + MOUSEINPUT's 32-byte
// union. SendInput validates cbSize against the real struct, so a wrong layout
// makes every call fail with ERROR_INVALID_PARAMETER rather than misbehaving
// subtly. This guard is load-bearing.
static_assert(sizeof(INPUT) == 40, "INPUT must be 40 bytes on x64");
#endif

namespace picopaste::win32 {
namespace {

constexpr WORD kKeyEventExtended = 0x0001;
constexpr WORD kKeyEventKeyUp = 0x0002;
constexpr LONG kKeyDownMask = static_cast<LONG>(0x8000);

bool IsKeyDown(INT vk) noexcept {
  return (GetAsyncKeyState(vk) & kKeyDownMask) != 0;
}

INPUT MakeKey(WORD vk, bool key_up, bool extended = false) noexcept {
  INPUT event{};
  event.type = INPUT_KEYBOARD;
  event.ki.wVk = vk;
  event.ki.wScan = 0;
  event.ki.dwFlags = (key_up ? kKeyEventKeyUp : 0) | (extended ? kKeyEventExtended : 0);
  event.ki.time = 0;
  event.ki.dwExtraInfo = 0;
  return event;
}

SendChordResult Submit(const INPUT* events, UINT count) noexcept {
  SendChordResult result{};
  result.requested = count;
  const UINT inserted = SendInput(count, const_cast<INPUT*>(events), sizeof(INPUT));
  result.inserted = static_cast<std::uint32_t>(inserted);
  result.ok = (inserted == count);
  result.last_error = result.ok ? 0u : GetLastError();
  return result;
}

// Read `path` into a clipboard HGLOBAL in one pass. Returns nullptr on any
// failure; on success the caller owns the returned HGLOBAL.
HGLOBAL LoadPngToHglobal(const wchar_t* path) noexcept {
  UniqueHandle file(
      CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  LARGE_INTEGER size{};
  if (file.valid() == false || GetFileSizeEx(file.get(), &size) == 0 || size.QuadPart <= 0) {
    return nullptr;
  }
  clip::win::Hglobal memory(static_cast<size_t>(size.QuadPart));
  if (static_cast<HGLOBAL>(memory) == nullptr) {
    return nullptr;
  }
  {
    clip::win::HglobalLock locked = memory.lock();
    if (locked == false) {
      return nullptr;
    }
    std::uint8_t* destination = locked.data<std::uint8_t*>();
    std::size_t offset = 0;
    const std::size_t total = static_cast<std::size_t>(size.QuadPart);
    while (offset < total) {
      const DWORD chunk = static_cast<DWORD>(total - offset > (1u << 20) ? (1u << 20) : (total - offset));
      DWORD got = 0;
      if (ReadFile(file.get(), destination + offset, chunk, &got, nullptr) == 0 || got == 0) {
        return nullptr;
      }
      offset += got;
    }
  }
  return memory.release();
}

// Put the remote path on the clipboard and type the paste chord into the
// baseline target. Restores the image before reporting a moved focus.
Status SendPasteToTarget(const PasteRequest& request) noexcept {
  std::string path_text(request.remote_path_utf8);
  if (clip::set_text(path_text) == false) {
    return Status::error(Error::kClipboardSetFailed);
  }

  if (request.delay_ms > 0) {
    Sleep(request.delay_ms);
  }

  if (GetForegroundWindow() != request.target_window) {
    if (request.restore_png_path != nullptr) {
      (void)SetClipboardPngFile(request.restore_png_path);
    }
    return Status::error(Error::kFocusChanged);
  }

  const SendChordResult chord = SendPasteChord();
  if (chord.ok == false) {
    return Status::error(Error::kSendInputRejected);
  }
  return Status::success();
}

}  // namespace

HWND CaptureForegroundWindow() noexcept {
  return GetForegroundWindow();
}

SendChordResult SendPasteChord() noexcept {
  // A physically held Alt or Win merges into the synthetic chord
  // (Ctrl+Alt+Shift+V or a Win chord) and the terminal does not treat it as
  // paste. Release those first; Shift and Ctrl are part of the chord anyway.
  //
  // VK_MENU is the generic Alt and maps to the non-extended (left) scan code,
  // so it cannot clear a held right Alt; VK_LMENU/VK_RMENU name the two keys.
  // The right Alt is an extended key (0xE0 scan prefix) and needs
  // KEYEVENTF_EXTENDEDKEY; the left Alt does not. (See "Extended-Key Flag" in
  // the Keyboard Input overview.)
  struct HeldModifier {
    WORD vk;
    bool extended;
  };
  constexpr HeldModifier kHeldModifiers[] = {
      {VK_LMENU, false},
      {VK_RMENU, true},
      {VK_LWIN, false},
      {VK_RWIN, false},
  };
  constexpr UINT kHeldModifierCount = static_cast<UINT>(sizeof(kHeldModifiers) / sizeof(kHeldModifiers[0]));
  INPUT release[kHeldModifierCount];
  UINT release_count = 0;
  for (UINT i = 0; i < kHeldModifierCount; ++i) {
    if (IsKeyDown(kHeldModifiers[i].vk)) {
      release[release_count] = MakeKey(kHeldModifiers[i].vk, true, kHeldModifiers[i].extended);
      ++release_count;
    }
  }
  if (release_count > 0) {
    const SendChordResult released = Submit(release, release_count);
    if (released.ok == false) {
      return released;
    }
    // Let the release settle so it is not observed out of order with the chord.
    Sleep(15);
  }

  INPUT chord[6];
  chord[0] = MakeKey(VK_CONTROL, false);
  chord[1] = MakeKey(VK_SHIFT, false);
  chord[2] = MakeKey(0x56, false);  // 'V'
  chord[3] = MakeKey(0x56, true);
  chord[4] = MakeKey(VK_SHIFT, true);
  chord[5] = MakeKey(VK_CONTROL, true);
  return Submit(chord, 6u);
}

Status SetClipboardPngFile(const wchar_t* path) noexcept {
  if (path == nullptr) {
    return Status::error(Error::kClipboardSetFailed);
  }
  const UINT png_format = RegisterClipboardFormatW(L"PNG");
  clip::win::Hglobal memory(LoadPngToHglobal(path));
  if (png_format == 0u || static_cast<HGLOBAL>(memory) == nullptr) {
    return Status::error(Error::kClipboardSetFailed);
  }

  clip::lock clipboard;
  if (clipboard.locked() == false) {
    return Status::error(Error::kClipboardOpenFailed);
  }
  (void)clipboard.clear();
  if (memory.set_clipboard_data(png_format) == false) {
    return Status::error(Error::kClipboardSetFailed);
  }
  return Status::success();
}

Status DeliverPaste(const PasteRequest& request) noexcept {
  if (request.remote_path_utf8 == nullptr) {
    return Status::error(Error::kClipboardSetFailed);
  }
  if (request.target_window == nullptr) {
    // No baseline: with no known target we must not type into whatever happens
    // to be focused.
    return Status::error(Error::kFocusChanged);
  }

  const Status delivered = SendPasteToTarget(request);
  if (delivered.has_value() == false) {
    return delivered;
  }

  if (request.restore_png_path != nullptr) {
    Sleep(150);
    // A restore failure must not turn a delivered paste into a reported failure.
    (void)SetClipboardPngFile(request.restore_png_path);
  }
  return Status::success();
}

}  // namespace picopaste::win32

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
 * @file tray.cpp
 * @brief Tray icon implementation.
 */
#include "tray.hpp"

#include "single_instance.hpp"
#include "stream_win32.hpp"
#include "tray_icons.h"
#include "win32_util.hpp"

#include <imm.h>
#include <shellapi.h>

namespace picopaste::win32 {
namespace {

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kMenuQuit = 1;
constexpr UINT kMenuRestart = 2;
constexpr wchar_t kWindowClass[] = L"picopaste-tray";

// How long a relaunched instance is given to prove it got past its own start-up.
// The child cannot take the single-instance mutex while this process still
// holds it, so a healthy child is still running when the probe expires; a child
// that has already exited by then refused to start. The probe blocks the main
// thread (it runs inside the WM_COMMAND handler), so it stays short: a child
// that failed to start does so within milliseconds, and one that is merely slow
// is indistinguishable from healthy at any length worth waiting.
constexpr DWORD kRelaunchProbeMs = 300;

// Three colour-coded icons: blue for healthy, amber for warning, red for error.
// They are this executable's own resources (see picopaste.rc.in; the ids live in
// tray_icons.h), so the tray does not depend on which stock system icons a given
// Windows version happens to ship. LoadIconW returns a shared handle, which must
// NOT be passed to DestroyIcon.
HICON MakeStateIcon(HINSTANCE instance, TrayState state) noexcept {
  std::int32_t resource = PICOPASTE_ICON_OK;
  switch (state) {
    case TrayState::kHealthy:
      resource = PICOPASTE_ICON_OK;
      break;
    case TrayState::kWarning:
      resource = PICOPASTE_ICON_WARN;
      break;
    case TrayState::kError:
      resource = PICOPASTE_ICON_ERR;
      break;
    default:
      resource = PICOPASTE_ICON_OK;
      break;
  }
  return LoadIconW(instance, MAKEINTRESOURCEW(resource));
}

// Relaunch this executable with the command line it was started with.
//
// Returns true only when a replacement process is up. The child is expected to
// still be running at the end of the probe: it is waiting for this process to
// release the single-instance mutex, which cannot happen until this process
// exits. The case worth catching is the opposite one -- a child that exits at
// once, because its start-up refused. Reporting that as a failed relaunch is
// what keeps "Restart" from turning into a silent exit.
bool RelaunchSelf() noexcept {
  wchar_t image[kOwnerImageChars] = {};
  const DWORD image_chars = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(kOwnerImageChars));
  if (image_chars == 0 || image_chars >= kOwnerImageChars) {
    return false;
  }

  // CreateProcessW may write to the command-line buffer, so it must be a copy of
  // this process's own line rather than GetCommandLineW()'s pointer. A line that
  // does not fit is refused rather than truncated: dropping an option silently is
  // the failure mode this project exists to remove.
  wchar_t command[kMaxCommandLineChars] = {};
  if (false == CopyWide(GetCommandLineW(), command, kMaxCommandLineChars)) {
    return false;
  }

  // A child inherits a snapshot of this environment at creation, so the wait
  // request only has to outlive the CreateProcessW call. The flag is what stops
  // the child from finding our mutex and exiting as a duplicate.
  (void)SetEnvironmentVariableW(kAwaitInstanceEnvName, L"1");

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  // Break away from the inherited memory job: a process already in a job cannot
  // join an unrelated second one, so without this the child fails in
  // SetupJobObjects and exits. The job grants the escape explicitly
  // (JOB_OBJECT_LIMIT_BREAKAWAY_OK) and only on request, so ssh.exe -- which
  // never asks -- stays inside the memory ceiling.
  const BOOL started = CreateProcessW(image, command, nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr,
                                      nullptr, &startup, &child);
  (void)SetEnvironmentVariableW(kAwaitInstanceEnvName, nullptr);
  if (started == 0) {
    return false;
  }

  const DWORD state = WaitForSingleObject(child.hProcess, kRelaunchProbeMs);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  return state == WAIT_TIMEOUT;
}

}  // namespace

Status Tray::Create(HINSTANCE instance, const wchar_t* tooltip) noexcept {
  if (hwnd_ != nullptr) {
    return Status::success();
  }

  // Keep the input-method framework out of this process.
  //
  // picopaste has no text-input UI, but creating a window is enough for an IME to
  // attach a context to this thread — and once attached, the IME's own code runs
  // on this thread's message path. That path is what crashed the process
  // repeatedly: an access violation inside the IME's frames (Sogou/TSF on this
  // machine), a minute or two after an upload, with nothing logged. With the IME
  // kept out, the same uploads run clean. -1 covers every thread in the process.
  constexpr DWORD kAllThreads = static_cast<DWORD>(-1);
  (void)ImmDisableIME(kAllThreads);

  icons_[0] = MakeStateIcon(instance, TrayState::kHealthy);
  icons_[1] = MakeStateIcon(instance, TrayState::kWarning);
  icons_[2] = MakeStateIcon(instance, TrayState::kError);
  if (icons_[0] == nullptr || icons_[1] == nullptr || icons_[2] == nullptr) {
    return Status::error(Error::kTempFileFailed);
  }

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = 0;
  window_class.lpfnWndProc = &Tray::StaticWndProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    Destroy();
    return Status::error(Error::kTempFileFailed);
  }

  // Hidden top-level window (no WS_VISIBLE). Broadcast messages reach it;
  // message-only windows would not receive TaskbarCreated.
  hwnd_ = CreateWindowExW(0, kWindowClass, L"picopaste", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    Destroy();
    return Status::error(Error::kTempFileFailed);
  }
  callback_message_ = kTrayCallback;
  taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

  if (tooltip != nullptr && tooltip[0] != L'\0') {
    SetState(TrayState::kHealthy, tooltip);
  }
  return Status::success();
}

Status Tray::Show() noexcept {
  if (hwnd_ == nullptr) {
    return Status::error(Error::kTempFileFailed);
  }
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = 1;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = callback_message_;
  data.hIcon = icons_[static_cast<int>(state_)];
  const wchar_t* tip = L"picopaste";
  (void)CopyWide(tip, data.szTip, 128);
  if (Shell_NotifyIconW(NIM_ADD, &data) == 0) {
    return Status::error(Error::kTempFileFailed);
  }
  visible_ = true;
  return Status::success();
}

void Tray::Destroy() noexcept {
  if (hwnd_ != nullptr && visible_) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = 1;
    (void)Shell_NotifyIconW(NIM_DELETE, &data);
    visible_ = false;
  }
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  // The icons are shared resource handles (LoadIconW caches them); DestroyIcon
  // must not be called on them. Clearing the handles is enough.
  for (std::int32_t i = 0; i < 3; ++i) {
    icons_[i] = nullptr;
  }
}

void Tray::FillTip(NOTIFYICONDATAW* data, const wchar_t* tooltip) noexcept {
  if (tooltip != nullptr) {
    (void)CopyWide(tooltip, data->szTip, 128);
  } else {
    (void)CopyWide(L"picopaste", data->szTip, 128);
  }
}

void Tray::SetState(TrayState state, const wchar_t* tooltip) noexcept {
  state_ = state;
  if (hwnd_ == nullptr || visible_ == false) {
    return;
  }
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = 1;
  data.uFlags = NIF_ICON | NIF_TIP;
  data.hIcon = icons_[static_cast<int>(state_)];
  FillTip(&data, tooltip);
  (void)Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::Notify(const wchar_t* title, const wchar_t* text, bool is_error) noexcept {
  if (hwnd_ == nullptr || visible_ == false) {
    return;
  }
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd_;
  data.uID = 1;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = is_error ? NIIF_ERROR : NIIF_INFO;
  if (title != nullptr) {
    (void)CopyWide(title, data.szInfoTitle, 64);
  }
  if (text != nullptr) {
    (void)CopyWide(text, data.szInfo, 256);
  }
  (void)Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::RequestQuit() noexcept {
  if (hwnd_ != nullptr) {
    (void)PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }
}

void Tray::ShowContextMenu() noexcept {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  AppendMenuW(menu, MF_STRING, kMenuRestart, L"Restart picopaste");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuQuit, L"Exit picopaste");
  POINT cursor{};
  (void)GetCursorPos(&cursor);
  SetForegroundWindow(hwnd_);
  (void)TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd_, nullptr);
  (void)PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

LRESULT CALLBACK Tray::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Tray* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<Tray*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (self != nullptr) {
    return self->WndProc(hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Tray::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept {
  if (taskbar_created_ != 0 && msg == taskbar_created_) {
    // Explorer restarted: the icon vanished. Re-add it.
    visible_ = false;
    (void)Show();
    return 0;
  }
  if (msg == callback_message_) {
    switch (lparam) {
      case WM_CONTEXTMENU:
      case WM_RBUTTONUP:
        ShowContextMenu();
        break;
      default:
        break;
    }
    return 0;
  }
  if (msg == WM_COMMAND) {
    if (LOWORD(wparam) == kMenuQuit) {
      PostQuitMessage(0);
    } else if (LOWORD(wparam) == kMenuRestart) {
      // Leave only when a replacement is actually running: a relaunch that
      // failed must not be indistinguishable from a successful restart.
      if (RelaunchSelf()) {
        PostQuitMessage(0);
      } else {
        Notify(L"picopaste", L"restart failed; still running", true);
      }
    }
    return 0;
  }
  if (msg == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace picopaste::win32

// picopaste -- tray icon implementation.
#include "tray.hpp"

#include <shellapi.h>

#include "win32_util.hpp"

namespace picopaste::win32 {
namespace {

constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kMenuQuit = 1;
constexpr wchar_t kWindowClass[] = L"picopaste-tray";

// Three colour-coded icons: blue for healthy, amber for warning, red for error.
// These are the shared system icons (LoadIconW with a null instance). They need
// no gdi32 drawing and survive correctly; DestroyIcon must NOT be called on
// them. (A hand-drawn green/amber/red set would require gdi32, which this
// target does not link.)
HICON MakeStateIcon(TrayState state) noexcept {
  switch (state) {
    case TrayState::kHealthy:
      return LoadIconW(nullptr, IDI_INFORMATION);  // blue "i"
    case TrayState::kWarning:
      return LoadIconW(nullptr, IDI_WARNING);  // amber triangle
    case TrayState::kError:
      return LoadIconW(nullptr, IDI_ERROR);  // red cross
    default:
      return LoadIconW(nullptr, IDI_APPLICATION);
  }
}

}  // namespace

Status Tray::Create(HINSTANCE instance, const wchar_t* tooltip) noexcept {
  if (hwnd_ != nullptr) {
    return Status::success();
  }
  icons_[0] = MakeStateIcon(TrayState::kHealthy);
  icons_[1] = MakeStateIcon(TrayState::kWarning);
  icons_[2] = MakeStateIcon(TrayState::kError);
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
  hwnd_ = CreateWindowExW(0, kWindowClass, L"picopaste", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                          nullptr, instance, this);
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
  // The icons are the shared system icons; DestroyIcon must not be called on
  // them. Clearing the handles is enough.
  for (int i = 0; i < 3; ++i) {
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
  AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit picopaste");
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

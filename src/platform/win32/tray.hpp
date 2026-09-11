// picopaste -- tray icon, balloon notifications and TaskbarCreated re-add.
//
// The tray window is a hidden *top-level* window, not a message-only window:
// Explorer broadcasts the registered TaskbarCreated message to top-level
// windows only, so an HWND_MESSAGE window would never see the restart and the
// icon would stay gone. Re-adding on that message is the fix for the previous
// implementation's lost-icon failure.
//
// Balloons are Shell_NotifyIconW NIF_INFO. No VBScript, PowerShell or
// WScript.Shell is involved anywhere.
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
#include <shellapi.h>

#include <cstdint>

#include "picopaste/error.hpp"

namespace picopaste::win32 {

enum class TrayState : std::uint8_t {
  kHealthy = 0,  // green
  kWarning = 1,  // amber
  kError = 2,    // red
};

class Tray final {
 public:
  Tray() noexcept = default;
  ~Tray() noexcept { Destroy(); }
  Tray(const Tray&) = delete;
  Tray& operator=(const Tray&) = delete;

  // Register the window class and create the hidden top-level window. `instance`
  // is the module handle; `tooltip` is the initial hover text.
  Status Create(HINSTANCE instance, const wchar_t* tooltip) noexcept;

  // Shell_NotifyIconW NIM_ADD. Safe to call again after a TaskbarCreated
  // broadcast (the message handler calls it for you).
  Status Show() noexcept;

  // NIM_DELETE, destroy the window and free the icons. Idempotent.
  void Destroy() noexcept;

  bool visible() const noexcept { return visible_; }
  HWND hwnd() const noexcept { return hwnd_; }

  // Switch the icon colour and hover text (NIM_MODIFY).
  void SetState(TrayState state, const wchar_t* tooltip) noexcept;

  // Balloon via NIF_INFO. `is_error` selects NIIF_ERROR over NIIF_INFO.
  void Notify(const wchar_t* title, const wchar_t* text, bool is_error) noexcept;

  // Ask the owning thread's message loop to exit (posts WM_CLOSE).
  void RequestQuit() noexcept;

 private:
  static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept;

  void ShowContextMenu() noexcept;
  void FillTip(NOTIFYICONDATAW* data, const wchar_t* tooltip) noexcept;

  HWND hwnd_ = nullptr;
  HICON icons_[3] = {nullptr, nullptr, nullptr};
  TrayState state_ = TrayState::kHealthy;
  UINT taskbar_created_ = 0;
  UINT callback_message_ = 0;
  bool visible_ = false;
};

}  // namespace picopaste::win32

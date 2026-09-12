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
 * @file tray.hpp
 * @brief Tray icon, balloon notifications and TaskbarCreated re-add.
 *
 * The tray window is a hidden *top-level* window, not a message-only window:
 * Explorer broadcasts the registered TaskbarCreated message to top-level
 * windows only, so an HWND_MESSAGE window would never see the restart and the
 * icon would stay gone. Re-adding on that message is the fix for the previous
 * implementation's lost-icon failure.
 *
 * Balloons are Shell_NotifyIconW NIF_INFO. No VBScript, PowerShell or
 * WScript.Shell is involved anywhere.
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

#include <shellapi.h>

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

  /**
   * @brief Register the window class and create the hidden top-level window.
   * @param instance the module handle.
   * @param tooltip the initial hover text.
   */
  Status Create(HINSTANCE instance, const wchar_t* tooltip) noexcept;

  /**
   * @brief Shell_NotifyIconW NIM_ADD.
   *
   * Safe to call again after a TaskbarCreated broadcast (the message handler
   * calls it for you).
   */
  Status Show() noexcept;

  /** @brief NIM_DELETE, destroy the window and free the icons. Idempotent. */
  void Destroy() noexcept;

  bool visible() const noexcept { return visible_; }
  HWND hwnd() const noexcept { return hwnd_; }

  /** @brief Switch the icon colour and hover text (NIM_MODIFY). */
  void SetState(TrayState state, const wchar_t* tooltip) noexcept;

  /**
   * @brief Balloon via NIF_INFO.
   * @param is_error selects NIIF_ERROR over NIIF_INFO.
   */
  void Notify(const wchar_t* title, const wchar_t* text, bool is_error) noexcept;

  /** @brief Ask the owning thread's message loop to exit (posts WM_CLOSE). */
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

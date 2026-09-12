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
 * @file error.hpp
 * @brief Error vocabulary shared by every layer.
 *
 * Deliberately a flat enum with fixed-width underlying type: it crosses the
 * core/win32 boundary, is logged verbatim, and must stay POD.
 */
#pragma once

#include "osp/vocabulary.hpp"

#include <cstdint>

namespace picopaste {

enum class Error : std::uint8_t {
  kOk = 0,

  // Clipboard capture
  kNoImageInClipboard,
  kClipboardOpenFailed,
  kClipboardLockFailed,
  kClipboardReadFailed,  // a format listed by the clipboard refused to hand over its bytes
  kImageTooLarge,
  kPngEncodeFailed,
  kTempFileFailed,

  // SFTP channel
  kChannelNotConnected,
  kChannelSpawnFailed,
  kChannelWriteFailed,
  kChannelReadFailed,
  kChannelEof,
  kSftpInitFailed,
  kSftpProtocolError,  // malformed packet / unexpected type
  kSftpStatusError,    // server replied SSH_FXP_STATUS != OK
  kRealpathFailed,
  kMkdirFailed,
  kOpenFailed,
  kWriteFailed,
  kCloseFailed,
  kStatFailed,
  kRenameFailed,
  kRemoveFailed,
  kSizeMismatch,    // remote byte count != local byte count
  kBufferTooSmall,  // remote file larger than the caller's read buffer

  // Paste injection
  kClipboardSetFailed,
  kFocusChanged,       // foreground window moved: keystroke withheld
  kSendInputRejected,  // SendInput inserted fewer events than asked

  // Lifecycle
  kConfigParseFailed,
  kConfigWriteFailed,
  kSingleInstanceExists,
  kSingleInstanceCreateFailed,  // the named mutex itself could not be created
  kHotkeyRegisterFailed,
  kJobObjectFailed,
  kBusy,           // an upload is already in flight
  kUploadTimeout,  // an upload stayed in flight past upload_timeout_ms

  // Bounded logging
  kLogIoFailed,  // open/write/rotate/flush of the log file failed

  // Self-test
  kSelfTestFailed,  // one or more mandatory capabilities failed
};

template <typename V>
using Result = osp::expected<V, Error>;

using Status = osp::expected<void, Error>;

}  // namespace picopaste

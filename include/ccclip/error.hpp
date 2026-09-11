// cc-clip-cpp — error vocabulary shared by every layer.
//
// Deliberately a flat enum with fixed-width underlying type: it crosses the
// core/win32 boundary, is logged verbatim, and must stay POD.
#pragma once

#include <cstdint>

#include "osp/vocabulary.hpp"

namespace ccclip {

enum class Error : std::uint8_t {
  kOk = 0,

  // Clipboard capture
  kNoImageInClipboard,
  kClipboardOpenFailed,
  kClipboardLockFailed,
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
  kSftpProtocolError,   // malformed packet / unexpected type
  kSftpStatusError,     // server replied SSH_FXP_STATUS != OK
  kRealpathFailed,
  kMkdirFailed,
  kOpenFailed,
  kWriteFailed,
  kCloseFailed,
  kStatFailed,
  kRenameFailed,
  kRemoveFailed,
  kSizeMismatch,        // remote byte count != local byte count

  // Paste injection
  kClipboardSetFailed,
  kFocusChanged,        // foreground window moved: keystroke withheld
  kSendInputRejected,   // SendInput inserted fewer events than asked

  // Lifecycle
  kConfigParseFailed,
  kSingleInstanceExists,
  kHotkeyRegisterFailed,
  kJobObjectFailed,
  kBusy,                // an upload is already in flight
};

template <typename V>
using Result = osp::expected<V, Error>;

using Status = osp::expected<void, Error>;

}  // namespace ccclip

// cc-clip-cpp — SFTP v3 wire constants.
//
// Only the opcodes this project actually uses are listed. Values are fixed by
// draft-ietf-secsh-filexfer-02 (SFTP protocol version 3) and must not be
// renumbered.
#pragma once

#include <cstdint>

namespace ccclip::sftp {

// Packet types.
enum class Pkt : std::uint8_t {
  kInit = 1,
  kVersion = 2,
  kOpen = 3,
  kClose = 4,
  kRead = 5,
  kWrite = 6,
  kRemove = 13,
  kMkdir = 14,
  kRealpath = 16,
  kStat = 17,
  kRename = 18,
  kOpendir = 11,
  kReaddir = 12,

  kStatus = 101,
  kHandle = 102,
  kData = 103,
  kName = 104,
  kAttrs = 105,
};

// SSH_FXP_STATUS codes.
enum class FxStatus : std::uint32_t {
  kOk = 0,
  kEof = 1,
  kNoSuchFile = 2,
  kPermissionDenied = 3,
  kFailure = 4,
  kBadMessage = 5,
  kNoConnection = 6,
  kConnectionLost = 7,
  kOpUnsupported = 8,
};

// SSH_FXF_* open flags (pflags).
inline constexpr std::uint32_t kFxRead = 0x00000001u;
inline constexpr std::uint32_t kFxWrite = 0x00000002u;
inline constexpr std::uint32_t kFxAppend = 0x00000004u;
inline constexpr std::uint32_t kFxCreat = 0x00000008u;
inline constexpr std::uint32_t kFxTrunc = 0x00000010u;
inline constexpr std::uint32_t kFxExcl = 0x00000020u;

// SSH_FILEXFER_ATTR_* flags. A field is present ONLY when its bit is set;
// parsing by fixed offset reads rubbish (verified against OpenSSH sftp-server).
inline constexpr std::uint32_t kAttrSize = 0x00000001u;
inline constexpr std::uint32_t kAttrUidGid = 0x00000002u;
inline constexpr std::uint32_t kAttrPerms = 0x00000004u;
inline constexpr std::uint32_t kAttrAcmodTime = 0x00000008u;
inline constexpr std::uint32_t kAttrExtended = 0x80000000u;

// Protocol version we speak. OpenSSH's sftp-server answers v3.
inline constexpr std::uint32_t kProtocolVersion = 3;

// Default maximum SFTP data payload; keeps a WRITE well under the server's
// max packet while staying a single allocation.
inline constexpr std::uint32_t kWriteChunkBytes = 64u * 1024u;

// OpenSSH sftp-server implements RENAME as link()+unlink(), so renaming ONTO an
// existing path fails with kFailure. Verified locally: RENAME -> existing
// target returns FAILURE(4). Callers must treat a non-empty target as an error
// rather than assuming overwrite semantics.
//
// Likewise MKDIR on an existing directory returns kFailure, NOT kOk. Verified
// locally. Directory creation must tolerate kFailure when the path exists.

}  // namespace ccclip::sftp

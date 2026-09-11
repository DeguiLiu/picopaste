// cc-clip-cpp — SFTP v3 client surface.
//
// Implemented in src/core/sftp/client.cpp. Synchronous and single-threaded by
// contract: callers serialize access (the upload worker is the only caller),
// so no internal locking is needed.
#pragma once

#include <cstdint>

#include "ccclip/error.hpp"
#include "ccclip/sftp/protocol.hpp"
#include "ccclip/sftp/stream.hpp"
#include "osp/vocabulary.hpp"

namespace ccclip::sftp {

// Longest remote path this client will handle. Sized for a home directory plus
// a few nesting levels; overflow is reported, never truncated silently.
inline constexpr std::uint32_t kMaxPathBytes = 512;

using Path = osp::FixedString<kMaxPathBytes>;

// Result of a successful upload: where the file landed and how big it is.
struct UploadedFile {
  Path path{};
  std::uint64_t size = 0;
};

class Client {
 public:
  // `stream` must outlive the client. Not owned.
  explicit Client(ByteStream stream) noexcept : stream_(stream) {}

  // SSH_FXP_INIT / SSH_FXP_VERSION. Must be the first call.
  // Fails with kSftpInitFailed when the peer does not answer v3 — which is
  // also how a remote without the sftp subsystem surfaces here.
  Status Init() noexcept;

  // Canonicalize `path` (use "." for the subsystem's initial directory, which
  // OpenSSH sets to the user's home). Replaces the upstream HOME probe: same
  // connection, no extra handshake, no shell, and no SSH banner to filter.
  Result<Path> Realpath(const char* path) noexcept;

  // Create one directory level. `FxStatus::kFailure` maps to success when the
  // path already exists — OpenSSH returns FAILURE (not OK) for an existing
  // directory, verified locally. Missing parents still fail.
  Status Mkdir(const char* path) noexcept;

  // Create every missing level of `path` (mkdir -p semantics), one level at a
  // time. Tolerates already-existing levels.
  Status MkdirAll(const char* path) noexcept;

  // Stream `local_path` to `remote_path`: OPEN(WRITE|CREAT|TRUNC) -> WRITE
  // chunks -> CLOSE -> STAT. Fails with kSizeMismatch if the remote byte count
  // differs from the local one, in which case the caller must NOT publish the
  // file (no RENAME has happened yet).
  Status UploadFile(const char* remote_path, const char* local_path) noexcept;

  // Size from SSH_FXP_STAT. Parses the ATTRS bitmask rather than assuming a
  // fixed layout; kStatFailed if the server omitted kAttrSize.
  Result<std::uint64_t> StatSize(const char* path) noexcept;

  // SSH_FXP_RENAME. NOT overwrite-capable on OpenSSH: renaming onto an
  // existing path fails with kFailure. Callers publish under fresh unique
  // names precisely so this cannot happen.
  Status Rename(const char* from, const char* to) noexcept;

  Status Remove(const char* path) noexcept;

  // True once Init() has completed against a v3 peer.
  bool initialized() const noexcept { return initialized_; }

 private:
  Status SendPacket(Pkt type, const void* payload, std::uint32_t len) noexcept;

  ByteStream stream_{};
  bool initialized_ = false;
  std::uint32_t next_id_ = 1;
};

}  // namespace ccclip::sftp

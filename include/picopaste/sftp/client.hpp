// picopaste — SFTP v3 client surface.
//
// This is the single owner of the SFTP channel: every wire-protocol operation
// (INIT, REALPATH, MKDIR, OPEN/READ/WRITE/CLOSE, OPENDIR/READDIR, STAT, RENAME,
// REMOVE) goes through one request-id counter and one frame codec, both private
// to this class. No other layer speaks the wire protocol or holds an id.
//
// Implemented in src/core/sftp/client.cpp. Synchronous and single-threaded by
// contract: callers serialize access (the upload worker is the only caller),
// so no internal locking is needed.
#pragma once

#include <cstdint>

#include "picopaste/error.hpp"
#include "picopaste/sftp/protocol.hpp"
#include "picopaste/sftp/stream.hpp"
#include "osp/vocabulary.hpp"

namespace picopaste::sftp {

// Longest remote path this client will handle. Sized for a home directory plus
// a few nesting levels; overflow is reported, never truncated silently.
inline constexpr std::uint32_t kMaxPathBytes = 512;

using Path = osp::FixedString<kMaxPathBytes>;

// Result of a successful upload: where the file landed and how big it is.
struct UploadedFile {
  Path path{};
  std::uint64_t size = 0;
};

// ---------------------------------------------------------------------------
// Directory listing surface (consumed by the retention policy in dir_ops.hpp).
//
// ListDir materialises a bounded snapshot of one remote directory, keeping only
// names that match our own upload pattern. A directory full of foreign files
// therefore costs O(1) slots; the pattern and the bounded containers live here
// because this is the one listing the product ever asks for.
// ---------------------------------------------------------------------------

// Uploaded names are `clip-YYYYMMDD-HHMMSS-<hex>.png` (design §3). The hex
// suffix is lowercase: the producer is this project, and a lowercase-only
// pattern is the strictest full match that cannot touch a foreign file.
// Longest legal name: 5 + 8 + 1 + 6 + 1 + 64 + 4 = 89 bytes.
inline constexpr std::uint32_t kMaxUploadNameBytes = 89u;
inline constexpr std::uint32_t kMaxUploadHexDigits = 64u;

// Fixed capacity of a single listing. 512 matching entries ~= 53 KB, held in
// the caller; the directory is bounded by the retention policy, so a healthy
// uploads directory never approaches this. Hitting it means the policy has
// stopped working and is reported via UploadListing::truncated.
inline constexpr std::uint32_t kMaxListedEntries = 512u;

// Upper bound on entries parsed from one SSH_FXP_NAME frame. The frame is
// itself size-capped (Client::kRxBytes), so this is a second, explicit guard
// against a malicious or broken peer claiming an absurd count.
inline constexpr std::uint32_t kMaxEntriesPerFrame = 1024u;

// One matched upload. Only matching names are ever stored here.
struct DirEntry {
  osp::FixedString<kMaxUploadNameBytes> name{};
  std::uint64_t size = 0u;
  std::uint32_t mtime = 0u;  // Unix seconds from SSH_FILEXFER_ATTRS.
  bool has_size = false;
  bool has_mtime = false;
};

// A bounded snapshot of one directory. `skipped` counts names that did not
// match the upload pattern (files, directories, anything foreign); they are
// left untouched and the caller is expected to surface the count.
struct UploadListing {
  osp::FixedVector<DirEntry, kMaxListedEntries> entries{};
  std::uint32_t skipped = 0u;
  bool truncated = false;    // cap reached: the listing is incomplete.
  bool dir_missing = false;  // OPENDIR answered NO_SUCH_FILE: nothing to do.
};

// Full-match test for our own upload filename. `name` need not be
// NUL-terminated; `len` bytes are examined and the pattern must consume all of
// them. Calendar fields are range-checked so a foreign `clip-...` file with an
// impossible date is not mistaken for ours.
bool MatchesUploadName(const char* name, std::uint32_t len) noexcept;

class Client {
 public:
  // Each instance owns its own scratch (below), so two Clients are
  // independent. They are still not thread-safe: the client is synchronous and
  // callers must serialize access, as documented on the class.
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

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

  // SSH_FXP_REMOVE. A NO_SUCH_FILE reply is success: "already gone" is the
  // desired end state for a cleanup, so a pass racing another cleanup does not
  // report failure for a file that has already vanished.
  Status Remove(const char* path) noexcept;

  // OPENDIR -> READDIR* -> CLOSE. Materialises a bounded listing of upload
  // names. A missing directory yields a successful listing with dir_missing
  // set, not an error. Hitting kMaxListedEntries sets truncated and stops.
  Result<UploadListing> ListDir(const char* dir) noexcept;

  // Read a remote file into caller-owned memory. `buffer` must hold
  // `capacity` bytes; `size` receives the byte count actually read. A missing
  // file is not an error: `size` is 0 and `missing` is set. If the file is
  // larger than `capacity` the read stops with kBufferTooSmall rather than
  // silently truncating, and the handle is closed.
  Status ReadFile(const char* path, std::uint8_t* buffer, std::uint32_t capacity,
                  std::uint32_t& size, bool& missing) noexcept;

  // OPEN(WRITE|CREAT|TRUNC) -> WRITE* -> CLOSE over caller-owned bytes. The
  // caller is responsible for backup/replace semantics; this writes exactly
  // the bytes it is given. `length` of 0 creates an empty file.
  Status WriteFile(const char* path, const std::uint8_t* bytes, std::uint32_t length) noexcept;

  // True once Init() has completed against a v3 peer.
  bool initialized() const noexcept { return initialized_; }

 private:
  Status SendPacket(Pkt type, const void* payload, std::uint32_t len) noexcept;

  // Sends OPEN/OPENDIR and parses the reply into `handle`. `missing` is set
  // when the server answers NO_SUCH_FILE (a normal state for a first write or
  // a not-yet-created directory). Shared by every open-style request so the
  // HANDLE decoding exists once.
  Status OpenHandle(Pkt request, const char* path, const void* extra, std::uint32_t extra_len,
                    std::uint8_t* handle, std::uint32_t& handle_len, bool& missing) noexcept;

  // SSH_FXP_CLOSE. Same packet for file and directory handles.
  Status CloseHandle(const std::uint8_t* handle, std::uint32_t handle_len) noexcept;

  // Per-instance scratch, sized from the codec's frame limits. The outbound
  // frame holds one full kWriteChunkBytes chunk plus its header fields. The
  // inbound frame must hold the largest reply: a READDIR NAME frame can reach
  // ~64 KB, and a READ reply is bounded by kReadChunkBytes. No heap; each
  // Client owns its own copy so instances cannot corrupt each other. Client is
  // ~128 KB and therefore not a small stack object.
  static constexpr std::uint32_t kTxBytes = kWriteChunkBytes + 512u;
  static constexpr std::uint32_t kRxBytes = 64u * 1024u;
  // Data requested per READ, kept below kRxBytes minus frame/id/string overhead
  // (60 KB + 9 B header fits a 64 KB frame with room to spare).
  static constexpr std::uint32_t kReadChunkBytes = 60u * 1024u;

  ByteStream stream_{};
  bool initialized_ = false;
  std::uint32_t next_id_ = 1;
  std::uint8_t tx_[kTxBytes]{};
  std::uint8_t rx_[kRxBytes]{};
};

}  // namespace picopaste::sftp

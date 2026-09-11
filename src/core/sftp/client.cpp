// picopaste — SFTP v3 client (see include/picopaste/sftp/client.hpp).
//
// Synchronous, single-threaded by contract. Scratch is per-instance (tx_/rx_):
// a 64 KB chunk plus a 16 KB inbound frame, reused for every request, never
// heap-allocated. Uploading a file is O(kWriteChunkBytes) memory and streams
// straight from the local file descriptor into the outbound WRITE payload.
#include "picopaste/sftp/client.hpp"

#include <cerrno>
#include <cstring>

#include "packet.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace picopaste::sftp {
namespace {

/* Byte offset inside a WRITE payload where the file chunk begins. */
inline constexpr std::uint32_t kWriteDataOffset(std::uint32_t handle_len) noexcept {
  return 4u /*id*/ + 4u /*handle len*/ + handle_len + 8u /*offset*/ + 4u /*data len*/;
}

// ---------------------------------------------------------------------------
// Local file I/O shim. The core is host-verified on Linux; MSVC gets the
// underscore-prefixed CRT equivalents behind the same small interface.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
using StatBuf = struct _stat64;
int LocalOpen(const char* path) noexcept { return ::_open(path, _O_RDONLY | _O_BINARY); }
int LocalClose(int fd) noexcept { return ::_close(fd); }
int LocalFstat(int fd, StatBuf* st) noexcept { return ::_fstat64(fd, st); }
long long LocalRead(int fd, void* buf, std::size_t len) noexcept {
  return static_cast<long long>(::_read(fd, buf, static_cast<unsigned int>(len)));
}
#else
using StatBuf = struct stat;
int LocalOpen(const char* path) noexcept { return ::open(path, O_RDONLY); }
int LocalClose(int fd) noexcept { return ::close(fd); }
int LocalFstat(int fd, StatBuf* st) noexcept { return ::fstat(fd, st); }
long long LocalRead(int fd, void* buf, std::size_t len) noexcept {
  return static_cast<long long>(::read(fd, buf, len));
}
#endif

// Reads until `len` bytes are read or EOF. Returns the number of bytes read
// (may be short only at EOF), or -1 on a hard error.
long long ReadFull(int fd, std::uint8_t* dst, std::size_t len) noexcept {
  std::size_t total = 0u;
  while (total < len) {
    const long long n = LocalRead(fd, dst + total, len - total);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      break;
    }
    total += static_cast<std::size_t>(n);
  }
  return static_cast<long long>(total);
}

// ---------------------------------------------------------------------------
// Frame I/O
// ---------------------------------------------------------------------------

// Reads one frame (length prefix + type + payload) into `rx`. Rejects a zero
// length, an oversized length, or a short read as a protocol error.
Error ReadFrame(ByteStream& s, std::uint8_t* rx, std::uint32_t rx_cap, std::uint8_t& type,
                std::uint32_t& payload_len) noexcept {
  std::uint8_t hdr[4];
  if (!s.read(s.ctx, hdr, sizeof(hdr))) {
    return Error::kChannelReadFailed;
  }
  const std::uint32_t frame_len = GetBe32(hdr);
  if ((frame_len < 1u) || (frame_len > rx_cap)) {
    return Error::kSftpProtocolError;
  }
  if (!s.read(s.ctx, &type, 1u)) {
    return Error::kSftpProtocolError;
  }
  payload_len = frame_len - 1u;
  if ((payload_len > 0u) && !s.read(s.ctx, rx, payload_len)) {
    return Error::kSftpProtocolError;
  }
  return Error::kOk;
}

// Validates the leading request id of a response payload and advances the
// reader past it. A mismatched id means the stream is out of sync.
Error ReadRequestId(BufferReader& r, std::uint32_t expect) noexcept {
  std::uint32_t id = 0u;
  if (!r.ReadU32(id)) {
    return Error::kSftpProtocolError;
  }
  if (id != expect) {
    return Error::kSftpProtocolError;
  }
  return Error::kOk;
}

// Parses the remainder of a STATUS payload (code, message, language) after the
// request id has already been consumed.
Error ParseStatusRest(BufferReader& r, FxStatus& code) noexcept {
  std::uint32_t raw = 0u;
  if (!r.ReadU32(raw)) {
    return Error::kSftpProtocolError;
  }
  const char* str = nullptr;
  std::uint32_t len = 0u;
  if (!r.ReadString(str, len) || !r.ReadString(str, len)) {
    return Error::kSftpProtocolError;
  }
  code = static_cast<FxStatus>(raw);
  return Error::kOk;
}

// Reads a response that must be STATUS. Returns kSftpProtocolError if the peer
// sent any other type or a mismatched id.
Error ReadStatus(ByteStream& s, std::uint8_t* rx, std::uint32_t rx_cap, std::uint32_t expect_id,
                 FxStatus& code) noexcept {
  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  const Error e = ReadFrame(s, rx, rx_cap, type, len);
  if (e != Error::kOk) {
    return e;
  }
  if (type != static_cast<std::uint8_t>(Pkt::kStatus)) {
    return Error::kSftpProtocolError;
  }
  BufferReader r(rx, len);
  const Error id_err = ReadRequestId(r, expect_id);
  if (id_err != Error::kOk) {
    return id_err;
  }
  return ParseStatusRest(r, code);
}

}  // namespace

// ---------------------------------------------------------------------------
// Packet emission
// ---------------------------------------------------------------------------

Status Client::SendPacket(Pkt type, const void* payload, std::uint32_t len) noexcept {
  if (!stream_.valid()) {
    return Status::error(Error::kChannelNotConnected);
  }
  std::uint8_t hdr[kFrameHeaderBytes];
  PutBe32(hdr, len + 1u);
  hdr[4] = static_cast<std::uint8_t>(type);
  if (!stream_.write(stream_.ctx, hdr, sizeof(hdr))) {
    return Status::error(Error::kChannelWriteFailed);
  }
  if ((len > 0u) && !stream_.write(stream_.ctx, static_cast<const std::uint8_t*>(payload), len)) {
    return Status::error(Error::kChannelWriteFailed);
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// INIT / VERSION
// ---------------------------------------------------------------------------

Status Client::Init() noexcept {
  if (!stream_.valid()) {
    return Status::error(Error::kChannelNotConnected);
  }
  std::uint8_t payload[4];
  PutBe32(payload, kProtocolVersion);
  const Status sent = SendPacket(Pkt::kInit, payload, sizeof(payload));
  if (!sent) {
    return Status::error(Error::kSftpInitFailed);
  }

  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  const Error e = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
  if (e != Error::kOk) {
    /* EOF here is the normal shape of "no sftp subsystem on the remote". */
    return Status::error(Error::kSftpInitFailed);
  }
  if (type != static_cast<std::uint8_t>(Pkt::kVersion)) {
    return Status::error(Error::kSftpInitFailed);
  }
  BufferReader r(rx_, len);
  std::uint32_t version = 0u;
  if (!r.ReadU32(version) || (version != kProtocolVersion)) {
    return Status::error(Error::kSftpInitFailed);
  }
  initialized_ = true;
  return Status::success();
}

// ---------------------------------------------------------------------------
// REALPATH
// ---------------------------------------------------------------------------

Result<Path> Client::Realpath(const char* path) noexcept {
  if (!initialized_) {
    return Result<Path>::error(Error::kChannelNotConnected);
  }
  const std::uint32_t id = next_id_++;
  BufferWriter w(tx_, sizeof(tx_));
  (void)w.WriteU32(id);
  if (!w.WriteCString(path, kMaxPathBytes)) {
    return Result<Path>::error(Error::kRealpathFailed);
  }
  const Status sent = SendPacket(Pkt::kRealpath, tx_, static_cast<std::uint32_t>(w.size()));
  if (!sent) {
    return Result<Path>::error(Error::kRealpathFailed);
  }

  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  const Error e = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
  if (e != Error::kOk) {
    return Result<Path>::error(Error::kRealpathFailed);
  }
  BufferReader r(rx_, len);
  const Error id_err = ReadRequestId(r, id);
  if (id_err != Error::kOk) {
    return Result<Path>::error(Error::kSftpProtocolError);
  }
  if (type == static_cast<std::uint8_t>(Pkt::kStatus)) {
    FxStatus code = FxStatus::kFailure;
    if (ParseStatusRest(r, code) != Error::kOk) {
      return Result<Path>::error(Error::kSftpProtocolError);
    }
    return Result<Path>::error(Error::kRealpathFailed);
  }
  if (type != static_cast<std::uint8_t>(Pkt::kName)) {
    return Result<Path>::error(Error::kSftpProtocolError);
  }

  std::uint32_t count = 0u;
  if (!r.ReadU32(count) || (count == 0u)) {
    return Result<Path>::error(Error::kSftpProtocolError);
  }
  const char* name = nullptr;
  std::uint32_t name_len = 0u;
  const char* longname = nullptr;
  std::uint32_t longname_len = 0u;
  AttrsInfo attrs{};
  if (!r.ReadString(name, name_len) || !r.ReadString(longname, longname_len) || !ParseAttrs(r, attrs)) {
    return Result<Path>::error(Error::kSftpProtocolError);
  }
  if (name_len > Path::capacity()) {
    /* Refuse to truncate a remote path silently. */
    return Result<Path>::error(Error::kRealpathFailed);
  }
  const Path resolved(osp::TruncateToCapacity, name, name_len);
  return Result<Path>::success(resolved);
}

// ---------------------------------------------------------------------------
// MKDIR / MKDIRALL
// ---------------------------------------------------------------------------

Status Client::Mkdir(const char* path) noexcept {
  if (!initialized_) {
    return Status::error(Error::kChannelNotConnected);
  }
  const std::uint32_t id = next_id_++;
  BufferWriter w(tx_, sizeof(tx_));
  (void)w.WriteU32(id);
  (void)w.WriteCString(path, kMaxPathBytes);
  (void)w.WriteU32(0u); /* empty ATTRS */
  if (!w.ok()) {
    return Status::error(Error::kMkdirFailed);
  }
  const Status sent = SendPacket(Pkt::kMkdir, tx_, static_cast<std::uint32_t>(w.size()));
  if (!sent) {
    return Status::error(Error::kMkdirFailed);
  }

  FxStatus code = FxStatus::kFailure;
  const Error e = ReadStatus(stream_, rx_, sizeof(rx_), id, code);
  if (e != Error::kOk) {
    return Status::error(e == Error::kChannelReadFailed ? Error::kMkdirFailed : e);
  }
  if (code == FxStatus::kOk) {
    return Status::success();
  }

  /* OpenSSH answers FAILURE (not OK) when the directory already exists.
     Disambiguate with STAT: if the path resolves, the mkdir was a no-op.
     STAT succeeds as ATTRS (the normal case) or as STATUS on some servers. */
  if (code == FxStatus::kFailure) {
    const std::uint32_t stat_id = next_id_++;
    BufferWriter sw(tx_, sizeof(tx_));
    (void)sw.WriteU32(stat_id);
    if (!sw.WriteCString(path, kMaxPathBytes)) {
      return Status::error(Error::kMkdirFailed);
    }
    const Status stat_sent = SendPacket(Pkt::kStat, tx_, static_cast<std::uint32_t>(sw.size()));
    if (!stat_sent) {
      return Status::error(Error::kMkdirFailed);
    }
    std::uint8_t stat_type = 0u;
    std::uint32_t stat_len = 0u;
    const Error stat_err = ReadFrame(stream_, rx_, sizeof(rx_), stat_type, stat_len);
    if (stat_err != Error::kOk) {
      return Status::error(Error::kMkdirFailed);
    }
    BufferReader sr(rx_, stat_len);
    if (ReadRequestId(sr, stat_id) != Error::kOk) {
      return Status::error(Error::kSftpProtocolError);
    }
    if (stat_type == static_cast<std::uint8_t>(Pkt::kAttrs)) {
      return Status::success();
    }
    if (stat_type == static_cast<std::uint8_t>(Pkt::kStatus)) {
      FxStatus stat_code = FxStatus::kFailure;
      if (ParseStatusRest(sr, stat_code) != Error::kOk) {
        return Status::error(Error::kSftpProtocolError);
      }
      if (stat_code == FxStatus::kOk) {
        return Status::success();
      }
    }
    return Status::error(Error::kMkdirFailed);
  }
  return Status::error(Error::kMkdirFailed);
}

Status Client::MkdirAll(const char* path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return Status::error(Error::kMkdirFailed);
  }
  char cur[kMaxPathBytes];
  std::uint32_t cur_len = 0u;
  std::size_t i = 0u;
  if (path[0] == '/') {
    cur[cur_len] = '/';
    ++cur_len;
    i = 1u;
  }

  while (path[i] != '\0') {
    const std::size_t start = i;
    while ((path[i] != '\0') && (path[i] != '/')) {
      ++i;
    }
    const std::size_t comp_len = i - start;
    if (comp_len > 0u) {
      if ((cur_len > 0u) && (cur[cur_len - 1u] != '/')) {
        if ((cur_len + 1u) >= kMaxPathBytes) {
          return Status::error(Error::kMkdirFailed);
        }
        cur[cur_len] = '/';
        ++cur_len;
      }
      if ((cur_len + comp_len) >= kMaxPathBytes) {
        return Status::error(Error::kMkdirFailed);
      }
      (void)std::memcpy(cur + cur_len, path + start, comp_len);
      cur_len += static_cast<std::uint32_t>(comp_len);
      cur[cur_len] = '\0';
      const Status mk = Mkdir(cur);
      if (!mk) {
        return Status::error(Error::kMkdirFailed);
      }
    }
    if (path[i] == '/') {
      ++i;
    }
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// OPEN / WRITE / CLOSE / STAT
// ---------------------------------------------------------------------------

Status Client::UploadFile(const char* remote_path, const char* local_path) noexcept {
  const int fd = LocalOpen(local_path);
  if (fd < 0) {
    return Status::error(Error::kOpenFailed);
  }
  /* Single cleanup point; every early exit routes through `done`. */
  auto done = [fd](Status s) noexcept -> Status {
    (void)LocalClose(fd);
    return s;
  };

  StatBuf st{};
  if (LocalFstat(fd, &st) != 0) {
    return done(Status::error(Error::kOpenFailed));
  }
  const std::uint64_t local_size = static_cast<std::uint64_t>(st.st_size);

  /* OPEN(WRITE|CREAT|TRUNC) */
  const std::uint32_t open_id = next_id_++;
  char handle[kMaxHandleBytes];
  std::uint32_t handle_len = 0u;
  {
    BufferWriter w(tx_, sizeof(tx_));
    (void)w.WriteU32(open_id);
    if (!w.WriteCString(remote_path, kMaxPathBytes)) {
      return done(Status::error(Error::kOpenFailed));
    }
    (void)w.WriteU32(kFxWrite | kFxCreat | kFxTrunc);
    (void)w.WriteU32(0u); /* empty ATTRS */
    if (!w.ok()) {
      return done(Status::error(Error::kOpenFailed));
    }
    const Status sent = SendPacket(Pkt::kOpen, tx_, static_cast<std::uint32_t>(w.size()));
    if (!sent) {
      return done(Status::error(Error::kOpenFailed));
    }

    std::uint8_t type = 0u;
    std::uint32_t len = 0u;
    const Error e = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
    if (e != Error::kOk) {
      return done(Status::error(Error::kOpenFailed));
    }
    BufferReader r(rx_, len);
    if (ReadRequestId(r, open_id) != Error::kOk) {
      return done(Status::error(Error::kSftpProtocolError));
    }
    if (type == static_cast<std::uint8_t>(Pkt::kStatus)) {
      FxStatus code = FxStatus::kFailure;
      if (ParseStatusRest(r, code) != Error::kOk) {
        return done(Status::error(Error::kSftpProtocolError));
      }
      return done(Status::error(Error::kOpenFailed));
    }
    if (type != static_cast<std::uint8_t>(Pkt::kHandle)) {
      return done(Status::error(Error::kSftpProtocolError));
    }
    const char* h = nullptr;
    std::uint32_t h_len = 0u;
    if (!r.ReadString(h, h_len) || (h_len == 0u) || (h_len > kMaxHandleBytes)) {
      return done(Status::error(Error::kOpenFailed));
    }
    (void)std::memcpy(handle, h, h_len);
    handle_len = h_len;
  }

  /* WRITE chunks, streaming from the local fd directly into the frame. */
  const std::uint32_t data_off = kWriteDataOffset(handle_len);
  std::uint64_t offset = 0u;
  bool eof = false;
  while (!eof) {
    const long long n = ReadFull(fd, tx_ + data_off, kWriteChunkBytes);
    if (n < 0) {
      return done(Status::error(Error::kWriteFailed));
    }
    if (n == 0) {
      break;
    }
    const std::uint32_t chunk = static_cast<std::uint32_t>(n);
    const std::uint32_t write_id = next_id_++;

    /* Prefix is laid out before the already-read data region. */
    BufferWriter w(tx_, sizeof(tx_));
    (void)w.WriteU32(write_id);
    (void)w.WriteString(handle, handle_len);
    (void)w.WriteU64(offset);
    (void)w.WriteU32(chunk);
    if (!w.ok() || (w.size() != data_off)) {
      return done(Status::error(Error::kWriteFailed));
    }
    const Status sent = SendPacket(Pkt::kWrite, tx_, data_off + chunk);
    if (!sent) {
      return done(Status::error(Error::kWriteFailed));
    }
    FxStatus code = FxStatus::kFailure;
    const Error e = ReadStatus(stream_, rx_, sizeof(rx_), write_id, code);
    if ((e != Error::kOk) || (code != FxStatus::kOk)) {
      return done(Status::error(Error::kWriteFailed));
    }
    offset += chunk;
    if (static_cast<std::uint32_t>(n) < kWriteChunkBytes) {
      eof = true; /* short read means the descriptor hit EOF */
    }
  }

  /* CLOSE */
  {
    const std::uint32_t close_id = next_id_++;
    BufferWriter w(tx_, sizeof(tx_));
    (void)w.WriteU32(close_id);
    (void)w.WriteString(handle, handle_len);
    if (!w.ok()) {
      return done(Status::error(Error::kCloseFailed));
    }
    const Status sent = SendPacket(Pkt::kClose, tx_, static_cast<std::uint32_t>(w.size()));
    if (!sent) {
      return done(Status::error(Error::kCloseFailed));
    }
    FxStatus code = FxStatus::kFailure;
    const Error e = ReadStatus(stream_, rx_, sizeof(rx_), close_id, code);
    if ((e != Error::kOk) || (code != FxStatus::kOk)) {
      return done(Status::error(Error::kCloseFailed));
    }
  }

  /* STAT + local/remote size comparison. No RENAME happens here. */
  const Result<std::uint64_t> remote_size = StatSize(remote_path);
  if (!remote_size) {
    return done(Status::error(Error::kStatFailed));
  }
  if ((remote_size.value() != local_size) || (offset != local_size)) {
    return done(Status::error(Error::kSizeMismatch));
  }
  return done(Status::success());
}

Result<std::uint64_t> Client::StatSize(const char* path) noexcept {
  if (!initialized_) {
    return Result<std::uint64_t>::error(Error::kChannelNotConnected);
  }
  const std::uint32_t id = next_id_++;
  BufferWriter w(tx_, sizeof(tx_));
  (void)w.WriteU32(id);
  if (!w.WriteCString(path, kMaxPathBytes)) {
    return Result<std::uint64_t>::error(Error::kStatFailed);
  }
  const Status sent = SendPacket(Pkt::kStat, tx_, static_cast<std::uint32_t>(w.size()));
  if (!sent) {
    return Result<std::uint64_t>::error(Error::kStatFailed);
  }

  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  const Error e = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
  if (e != Error::kOk) {
    return Result<std::uint64_t>::error(Error::kStatFailed);
  }
  BufferReader r(rx_, len);
  if (ReadRequestId(r, id) != Error::kOk) {
    return Result<std::uint64_t>::error(Error::kSftpProtocolError);
  }
  if (type == static_cast<std::uint8_t>(Pkt::kStatus)) {
    FxStatus code = FxStatus::kFailure;
    if (ParseStatusRest(r, code) != Error::kOk) {
      return Result<std::uint64_t>::error(Error::kSftpProtocolError);
    }
    return Result<std::uint64_t>::error(Error::kStatFailed);
  }
  if (type != static_cast<std::uint8_t>(Pkt::kAttrs)) {
    return Result<std::uint64_t>::error(Error::kSftpProtocolError);
  }
  AttrsInfo attrs{};
  if (!ParseAttrs(r, attrs) || !attrs.has_size) {
    return Result<std::uint64_t>::error(Error::kStatFailed);
  }
  return Result<std::uint64_t>::success(attrs.size);
}

// ---------------------------------------------------------------------------
// RENAME / REMOVE
// ---------------------------------------------------------------------------

Status Client::Rename(const char* from, const char* to) noexcept {
  if (!initialized_) {
    return Status::error(Error::kChannelNotConnected);
  }
  const std::uint32_t id = next_id_++;
  BufferWriter w(tx_, sizeof(tx_));
  (void)w.WriteU32(id);
  if (!w.WriteCString(from, kMaxPathBytes) || !w.WriteCString(to, kMaxPathBytes)) {
    return Status::error(Error::kRenameFailed);
  }
  const Status sent = SendPacket(Pkt::kRename, tx_, static_cast<std::uint32_t>(w.size()));
  if (!sent) {
    return Status::error(Error::kRenameFailed);
  }
  /* OpenSSH implements RENAME as link()+unlink(): an existing target yields
     FAILURE(4). Treat every non-OK status as a failed rename. */
  FxStatus code = FxStatus::kFailure;
  const Error e = ReadStatus(stream_, rx_, sizeof(rx_), id, code);
  if ((e != Error::kOk) || (code != FxStatus::kOk)) {
    return Status::error(Error::kRenameFailed);
  }
  return Status::success();
}

Status Client::Remove(const char* path) noexcept {
  if (!initialized_) {
    return Status::error(Error::kChannelNotConnected);
  }
  const std::uint32_t id = next_id_++;
  BufferWriter w(tx_, sizeof(tx_));
  (void)w.WriteU32(id);
  if (!w.WriteCString(path, kMaxPathBytes)) {
    return Status::error(Error::kRemoveFailed);
  }
  const Status sent = SendPacket(Pkt::kRemove, tx_, static_cast<std::uint32_t>(w.size()));
  if (!sent) {
    return Status::error(Error::kRemoveFailed);
  }
  FxStatus code = FxStatus::kFailure;
  const Error e = ReadStatus(stream_, rx_, sizeof(rx_), id, code);
  if ((e != Error::kOk) || (code != FxStatus::kOk)) {
    return Status::error(Error::kRemoveFailed);
  }
  return Status::success();
}

}  // namespace picopaste::sftp

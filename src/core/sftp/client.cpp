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
 * @file client.cpp
 * @brief SFTP v3 client implementation.
 *
 * The single owner of the channel: every request is emitted and every reply
 * parsed here, against one request-id counter and one frame codec. Synchronous,
 * single-threaded by contract. Scratch is per-instance (tx_/rx_): a 64 KB
 * outbound chunk plus a 64 KB inbound frame, reused for every request, never
 * heap-allocated. Uploading a file streams straight from the local file
 * descriptor into the outbound WRITE payload.
 */
#include "picopaste/sftp/client.hpp"

#include "packet.hpp"

#include <cerrno>
#include <cstring>

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

// The stat buffer aliases must be declared at global scope. An
// elaborated-type-specifier (`struct _stat64` / `struct stat`) written inside a
// namespace declares a *new* incomplete type in that namespace rather than
// finding the one <sys/stat.h> defined at global scope: the alias would then
// name an incomplete local struct and every use of it would fail to compile.
// At global scope the same spelling finds the real type.
#if defined(_WIN32)
using StatBuf = struct _stat64;
#else
using StatBuf = struct stat;
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
std::int32_t LocalOpen(const char* path) noexcept {
  return ::_open(path, _O_RDONLY | _O_BINARY);
}
std::int32_t LocalClose(std::int32_t fd) noexcept {
  return ::_close(fd);
}
std::int32_t LocalFstat(std::int32_t fd, StatBuf* st) noexcept {
  return ::_fstat64(fd, st);
}
std::int64_t LocalRead(std::int32_t fd, void* buf, std::size_t len) noexcept {
  return static_cast<std::int64_t>(::_read(fd, buf, static_cast<unsigned int>(len)));
}
#else
std::int32_t LocalOpen(const char* path) noexcept {
  return ::open(path, O_RDONLY);
}
std::int32_t LocalClose(std::int32_t fd) noexcept {
  return ::close(fd);
}
std::int32_t LocalFstat(std::int32_t fd, StatBuf* st) noexcept {
  return ::fstat(fd, st);
}
std::int64_t LocalRead(std::int32_t fd, void* buf, std::size_t len) noexcept {
  return static_cast<std::int64_t>(::read(fd, buf, len));
}
#endif

// Reads until `len` bytes are read or EOF. Returns the number of bytes read
// (may be short only at EOF), or -1 on a hard error.
std::int64_t ReadFull(std::int32_t fd, std::uint8_t* dst, std::size_t len) noexcept {
  std::size_t total = 0u;
  while (total < len) {
    const std::int64_t n = LocalRead(fd, dst + total, len - total);
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
  return static_cast<std::int64_t>(total);
}

// ---------------------------------------------------------------------------
// Frame I/O (the one codec)
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

// ---------------------------------------------------------------------------
// Upload-name pattern (declared in client.hpp; the listing filter).
// ---------------------------------------------------------------------------

bool IsDigit(char c) noexcept {
  return (c >= '0') && (c <= '9');
}

bool IsLowerHex(char c) noexcept {
  return IsDigit(c) || ((c >= 'a') && (c <= 'f'));
}

std::uint32_t Dec2(const char* p) noexcept {
  return (static_cast<std::uint32_t>(p[0] - '0') * 10u) + static_cast<std::uint32_t>(p[1] - '0');
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
// Open / close primitives
// ---------------------------------------------------------------------------

Status Client::OpenHandle(Pkt request, const char* path, const void* extra, std::uint32_t extra_len,
                          std::uint8_t* handle, std::uint32_t& handle_len, bool& missing) noexcept {
  missing = false;
  handle_len = 0u;
  if (!initialized_) {
    return Status::error(Error::kChannelNotConnected);
  }
  const std::uint32_t id = next_id_++;
  BufferWriter w(tx_, sizeof(tx_));
  (void)w.WriteU32(id);
  if (!w.WriteCString(path, kMaxPathBytes)) {
    return Status::error(Error::kOpenFailed);
  }
  if ((extra_len > 0u) && !w.WriteBytes(extra, extra_len)) {
    return Status::error(Error::kOpenFailed);
  }
  if (!w.ok()) {
    return Status::error(Error::kOpenFailed);
  }
  if (!SendPacket(request, tx_, static_cast<std::uint32_t>(w.size()))) {
    return Status::error(Error::kOpenFailed);
  }

  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  const Error e = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
  if (e != Error::kOk) {
    return Status::error((e == Error::kChannelReadFailed) ? Error::kOpenFailed : e);
  }
  BufferReader r(rx_, len);
  if (ReadRequestId(r, id) != Error::kOk) {
    return Status::error(Error::kSftpProtocolError);
  }
  if (type == static_cast<std::uint8_t>(Pkt::kStatus)) {
    FxStatus code = FxStatus::kFailure;
    if (ParseStatusRest(r, code) != Error::kOk) {
      return Status::error(Error::kSftpProtocolError);
    }
    if (code == FxStatus::kNoSuchFile) {
      missing = true;
      return Status::success();
    }
    return Status::error(Error::kOpenFailed);
  }
  if (type != static_cast<std::uint8_t>(Pkt::kHandle)) {
    return Status::error(Error::kSftpProtocolError);
  }
  const char* h = nullptr;
  std::uint32_t h_len = 0u;
  if (!r.ReadString(h, h_len) || (h_len == 0u) || (h_len > kMaxHandleBytes)) {
    return Status::error(Error::kOpenFailed);
  }
  (void)std::memcpy(handle, h, h_len);
  handle_len = h_len;
  return Status::success();
}

Status Client::CloseHandle(const std::uint8_t* handle, std::uint32_t handle_len) noexcept {
  const std::uint32_t id = next_id_++;
  BufferWriter w(tx_, sizeof(tx_));
  (void)w.WriteU32(id);
  (void)w.WriteString(reinterpret_cast<const char*>(handle), handle_len);
  if (!w.ok()) {
    return Status::error(Error::kCloseFailed);
  }
  if (!SendPacket(Pkt::kClose, tx_, static_cast<std::uint32_t>(w.size()))) {
    return Status::error(Error::kCloseFailed);
  }
  FxStatus code = FxStatus::kFailure;
  const Error e = ReadStatus(stream_, rx_, sizeof(rx_), id, code);
  if ((e != Error::kOk) || (code != FxStatus::kOk)) {
    return Status::error(Error::kCloseFailed);
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
// OPEN / READ / WRITE / CLOSE / STAT
// ---------------------------------------------------------------------------

Status Client::UploadFile(const char* remote_path, const char* local_path) noexcept {
  const std::int32_t fd = LocalOpen(local_path);
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
  std::uint8_t extras[8];
  PutBe32(extras, kFxWrite | kFxCreat | kFxTrunc);
  PutBe32(extras + 4u, 0u); /* empty ATTRS */
  std::uint8_t handle[kMaxHandleBytes];
  std::uint32_t handle_len = 0u;
  bool missing = false;
  if (!OpenHandle(Pkt::kOpen, remote_path, extras, sizeof(extras), handle, handle_len, missing)) {
    return done(Status::error(Error::kOpenFailed));
  }

  /* WRITE chunks, streaming from the local fd directly into the frame. */
  const std::uint32_t data_off = kWriteDataOffset(handle_len);
  std::uint64_t offset = 0u;
  bool eof = false;
  while (!eof) {
    const std::int64_t n = ReadFull(fd, tx_ + data_off, kWriteChunkBytes);
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
    (void)w.WriteString(reinterpret_cast<const char*>(handle), handle_len);
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

  if (!CloseHandle(handle, handle_len)) {
    return done(Status::error(Error::kCloseFailed));
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

Status Client::ReadFile(const char* path, std::uint8_t* buffer, std::uint32_t capacity, std::uint32_t& size,
                        bool& missing) noexcept {
  size = 0u;
  missing = false;
  if ((path == nullptr) || ((buffer == nullptr) && (capacity > 0u))) {
    return Status::error(Error::kOpenFailed);
  }

  std::uint8_t handle[kMaxHandleBytes];
  std::uint32_t handle_len = 0u;
  std::uint8_t extras[8];
  PutBe32(extras, kFxRead);
  PutBe32(extras + 4u, 0u); /* empty ATTRS */
  const Status opened = OpenHandle(Pkt::kOpen, path, extras, sizeof(extras), handle, handle_len, missing);
  if (!opened) {
    return opened;
  }
  if (missing) {
    return Status::success();
  }
  auto fail = [&](Error e) noexcept -> Status {
    (void)CloseHandle(handle, handle_len);
    return Status::error(e);
  };

  std::uint64_t offset = 0u;
  for (;;) {
    const std::uint32_t remaining = capacity - size;
    /* When the buffer is full, ask for one byte so an oversized file is
       detected instead of silently truncated. */
    const std::uint32_t ask = (remaining == 0u) ? 1u : ((remaining < kReadChunkBytes) ? remaining : kReadChunkBytes);
    const std::uint32_t read_id = next_id_++;
    BufferWriter w(tx_, sizeof(tx_));
    (void)w.WriteU32(read_id);
    (void)w.WriteString(reinterpret_cast<const char*>(handle), handle_len);
    (void)w.WriteU64(offset);
    (void)w.WriteU32(ask);
    if (!w.ok()) {
      return fail(Error::kSftpProtocolError);
    }
    if (!SendPacket(Pkt::kRead, tx_, static_cast<std::uint32_t>(w.size()))) {
      return fail(Error::kChannelWriteFailed);
    }

    std::uint8_t type = 0u;
    std::uint32_t len = 0u;
    const Error e = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
    if (e != Error::kOk) {
      return fail(e);
    }
    BufferReader r(rx_, len);
    if (ReadRequestId(r, read_id) != Error::kOk) {
      return fail(Error::kSftpProtocolError);
    }
    if (type == static_cast<std::uint8_t>(Pkt::kStatus)) {
      FxStatus code = FxStatus::kFailure;
      if (ParseStatusRest(r, code) != Error::kOk) {
        return fail(Error::kSftpProtocolError);
      }
      if (code == FxStatus::kEof) {
        break;
      }
      return fail(Error::kOpenFailed);
    }
    if (type != static_cast<std::uint8_t>(Pkt::kData)) {
      return fail(Error::kSftpProtocolError);
    }
    const char* data = nullptr;
    std::uint32_t data_len = 0u;
    if (!r.ReadString(data, data_len)) {
      return fail(Error::kSftpProtocolError);
    }
    if (data_len == 0u) {
      break; /* no progress: stop rather than spin */
    }
    if (data_len > (capacity - size)) {
      return fail(Error::kBufferTooSmall); /* over capacity: refuse loudly */
    }
    (void)std::memcpy(buffer + size, data, data_len);
    size += data_len;
    offset += data_len;
  }
  return CloseHandle(handle, handle_len);
}

Status Client::WriteFile(const char* path, const std::uint8_t* bytes, std::uint32_t length) noexcept {
  if ((path == nullptr) || ((bytes == nullptr) && (length > 0u))) {
    return Status::error(Error::kWriteFailed);
  }
  std::uint8_t extras[8];
  /* OpenSSH RENAME is not overwrite-capable, so replace via OPEN(TRUNC). The
     caller has already taken a verified backup where reversibility matters. */
  PutBe32(extras, kFxWrite | kFxCreat | kFxTrunc);
  PutBe32(extras + 4u, 0u); /* empty ATTRS */
  std::uint8_t handle[kMaxHandleBytes];
  std::uint32_t handle_len = 0u;
  bool missing = false;
  const Status opened = OpenHandle(Pkt::kOpen, path, extras, sizeof(extras), handle, handle_len, missing);
  if (!opened) {
    return opened;
  }
  auto fail = [&](Error e) noexcept -> Status {
    (void)CloseHandle(handle, handle_len);
    return Status::error(e);
  };

  std::uint64_t offset = 0u;
  std::uint32_t pos = 0u;
  while (pos < length) {
    const std::uint32_t remaining = length - pos;
    const std::uint32_t chunk = (remaining < kWriteChunkBytes) ? remaining : kWriteChunkBytes;
    const std::uint32_t write_id = next_id_++;
    BufferWriter w(tx_, sizeof(tx_));
    (void)w.WriteU32(write_id);
    (void)w.WriteString(reinterpret_cast<const char*>(handle), handle_len);
    (void)w.WriteU64(offset);
    (void)w.WriteU32(chunk);
    if (!w.ok()) {
      return fail(Error::kWriteFailed);
    }
    const std::size_t data_off = w.size();
    if ((data_off + chunk) > sizeof(tx_)) {
      return fail(Error::kWriteFailed);
    }
    (void)std::memcpy(tx_ + data_off, bytes + pos, chunk);
    if (!SendPacket(Pkt::kWrite, tx_, static_cast<std::uint32_t>(data_off + chunk))) {
      return fail(Error::kChannelWriteFailed);
    }
    FxStatus code = FxStatus::kFailure;
    const Error e = ReadStatus(stream_, rx_, sizeof(rx_), write_id, code);
    if ((e != Error::kOk) || (code != FxStatus::kOk)) {
      return fail(Error::kWriteFailed);
    }
    pos += chunk;
    offset += chunk;
  }
  return CloseHandle(handle, handle_len);
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
  if (e != Error::kOk) {
    return Status::error(Error::kRemoveFailed);
  }
  /* A cleanup that races another cleanup must not report failure: "already
     gone" is the desired end state. */
  if ((code == FxStatus::kOk) || (code == FxStatus::kNoSuchFile)) {
    return Status::success();
  }
  return Status::error(Error::kRemoveFailed);
}

// ---------------------------------------------------------------------------
// OPENDIR / READDIR / CLOSE
// ---------------------------------------------------------------------------

Result<UploadListing> Client::ListDir(const char* dir) noexcept {
  UploadListing out{};
  if ((dir == nullptr) || (dir[0] == '\0')) {
    return Result<UploadListing>::error(Error::kChannelNotConnected);
  }

  std::uint8_t handle[kMaxHandleBytes];
  std::uint32_t handle_len = 0u;
  bool missing = false;
  const Status opened = OpenHandle(Pkt::kOpendir, dir, /*extra=*/nullptr, 0u, handle, handle_len, missing);
  if (!opened) {
    return Result<UploadListing>::error(opened.get_error());
  }
  if (missing) {
    out.dir_missing = true;
    return Result<UploadListing>::success(out);
  }

  /* Single close point: every path below joins here before returning. */
  Error err = Error::kOk;
  bool done = false;
  while (!done && (err == Error::kOk)) {
    const std::uint32_t id = next_id_++;
    BufferWriter w(tx_, sizeof(tx_));
    (void)w.WriteU32(id);
    (void)w.WriteString(reinterpret_cast<const char*>(handle), handle_len);
    if (!w.ok()) {
      err = Error::kSftpProtocolError;
      break;
    }
    if (!SendPacket(Pkt::kReaddir, tx_, static_cast<std::uint32_t>(w.size()))) {
      err = Error::kChannelWriteFailed;
      break;
    }

    std::uint8_t type = 0u;
    std::uint32_t len = 0u;
    const Error e = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
    if (e != Error::kOk) {
      err = e;
      break;
    }
    BufferReader r(rx_, len);
    if (ReadRequestId(r, id) != Error::kOk) {
      err = Error::kSftpProtocolError;
      break;
    }

    if (type == static_cast<std::uint8_t>(Pkt::kStatus)) {
      FxStatus code = FxStatus::kFailure;
      if (ParseStatusRest(r, code) != Error::kOk) {
        err = Error::kSftpProtocolError;
        break;
      }
      if (code == FxStatus::kEof) {
        done = true;
      } else {
        err = Error::kSftpStatusError;
      }
      continue;
    }
    if (type != static_cast<std::uint8_t>(Pkt::kName)) {
      err = Error::kSftpProtocolError;
      break;
    }

    std::uint32_t count = 0u;
    if (!r.ReadU32(count) || (count == 0u) || (count > kMaxEntriesPerFrame)) {
      err = Error::kSftpProtocolError;
      break;
    }
    for (std::uint32_t i = 0u; (i < count) && (err == Error::kOk); ++i) {
      /* longname and the full ATTRS must be consumed even for entries we skip,
         or the next NAME entry desynchronises. */
      const char* fname = nullptr;
      std::uint32_t fname_len = 0u;
      const char* longname = nullptr;
      std::uint32_t longname_len = 0u;
      AttrsInfo attrs{};
      if (!r.ReadString(fname, fname_len) || !r.ReadString(longname, longname_len) || !ParseAttrs(r, attrs)) {
        err = Error::kSftpProtocolError;
        break;
      }
      if (!MatchesUploadName(fname, fname_len)) {
        ++out.skipped;
        continue;
      }
      if (out.entries.full()) {
        out.truncated = true;
        done = true;
        break;
      }
      DirEntry entry{};
      entry.name = osp::FixedString<kMaxUploadNameBytes>(osp::TruncateToCapacity, fname, fname_len);
      entry.mtime = attrs.mtime;
      entry.has_mtime = attrs.has_acmod_time;
      entry.size = attrs.size;
      entry.has_size = attrs.has_size;
      if (!out.entries.push_back(entry)) {
        out.truncated = true;
        done = true;
        break;
      }
    }
  }

  const Status closed = CloseHandle(handle, handle_len);
  if (err != Error::kOk) {
    return Result<UploadListing>::error(err);
  }
  if (!closed) {
    return Result<UploadListing>::error(Error::kCloseFailed);
  }
  return Result<UploadListing>::success(out);
}

// ---------------------------------------------------------------------------
// Upload-name pattern
// ---------------------------------------------------------------------------

namespace {

bool AllDigits(const char* s, std::uint32_t first, std::uint32_t last) noexcept {
  for (std::uint32_t i = first; i <= last; ++i) {
    if (!IsDigit(s[i])) {
      return false;
    }
  }
  return true;
}

// "clip-" plus the two timestamp separators and their digit fields.
bool HasUploadPrefix(const char* name) noexcept {
  if (std::memcmp(name, "clip-", 5u) != 0) {
    return false;
  }
  if ((name[13] != '-') || (name[20] != '-')) {
    return false;
  }
  return AllDigits(name, 5u, 12u) && AllDigits(name, 14u, 19u);
}

// Month/day/hour/minute/second must be in range, so a name that merely looks
// like a timestamp is still rejected.
bool HasUploadTimestamp(const char* name) noexcept {
  const std::uint32_t month = Dec2(name + 9u);
  const std::uint32_t day = Dec2(name + 11u);
  const std::uint32_t hour = Dec2(name + 14u);
  const std::uint32_t minute = Dec2(name + 16u);
  const std::uint32_t second = Dec2(name + 18u);
  if ((month < 1u) || (month > 12u) || (day < 1u) || (day > 31u) || (hour > 23u) || (minute > 59u) || (second > 59u)) {
    return false;
  }
  return true;
}

// ".png" followed by a non-empty lowercase-hex suffix.
bool HasUploadTail(const char* name, std::uint32_t len) noexcept {
  const std::uint32_t suffix = len - 4u;
  if (std::memcmp(name + suffix, ".png", 4u) != 0) {
    return false;
  }
  const std::uint32_t hex_len = suffix - 21u;
  if ((hex_len < 1u) || (hex_len > kMaxUploadHexDigits)) {
    return false;
  }
  for (std::uint32_t i = 21u; i < suffix; ++i) {
    if (!IsLowerHex(name[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool MatchesUploadName(const char* name, std::uint32_t len) noexcept {
  /* clip-YYYYMMDD-HHMMSS-<hex>.png */
  constexpr std::uint32_t kMinLen = 5u + 8u + 1u + 6u + 1u + 1u + 4u; /* 26 */
  if ((name == nullptr) || (len < kMinLen) || (len > kMaxUploadNameBytes)) {
    return false;
  }
  if (!HasUploadPrefix(name) || !HasUploadTimestamp(name) || !HasUploadTail(name, len)) {
    return false;
  }
  return true;
}

}  // namespace picopaste::sftp

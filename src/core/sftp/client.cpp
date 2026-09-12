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
// Shared request helpers. A file-local free function cannot reach the private
// Client members, so frame emission and the WRITE-chunk exchange live here and
// the client methods call into them rather than duplicating the wire layout.
// ---------------------------------------------------------------------------

// Writes one frame (length prefix + type + payload) onto the channel.
Error SendFrame(ByteStream& s, Pkt type, const void* payload, std::uint32_t len) noexcept {
  if (!s.valid()) {
    return Error::kChannelNotConnected;
  }
  std::uint8_t hdr[kFrameHeaderBytes];
  PutBe32(hdr, len + 1u);
  hdr[4] = static_cast<std::uint8_t>(type);
  if (!s.write(s.ctx, hdr, sizeof(hdr))) {
    return Error::kChannelWriteFailed;
  }
  if ((len > 0u) && !s.write(s.ctx, static_cast<const std::uint8_t*>(payload), len)) {
    return Error::kChannelWriteFailed;
  }
  return Error::kOk;
}

// Builds the WRITE prefix in `tx` and sends the frame, then waits for the
// per-WRITE OK ack. The chunk bytes must already sit in `tx` at the offset
// kWriteDataOffset(handle_len), because the prefix is written around them.
// `send_err` is the caller's operation-specific name for a failed frame write
// (WriteFile surfaces a channel error; UploadFile folds it into kWriteFailed).
Error SendWriteChunk(ByteStream& s, std::uint8_t* tx, std::size_t tx_cap, std::uint32_t id, const std::uint8_t* handle,
                     std::uint32_t handle_len, std::uint64_t offset, std::uint32_t data_len, std::uint8_t* rx,
                     std::uint32_t rx_cap, Error send_err) noexcept {
  const std::uint32_t data_off = kWriteDataOffset(handle_len);
  if ((data_off + data_len) > tx_cap) {
    return Error::kWriteFailed;
  }
  BufferWriter w(tx, tx_cap);
  (void)w.WriteU32(id);
  (void)w.WriteString(reinterpret_cast<const char*>(handle), handle_len);
  (void)w.WriteU64(offset);
  (void)w.WriteU32(data_len);
  if (!w.ok() || (w.size() != data_off)) {
    return Error::kWriteFailed;
  }
  if (Error::kOk != SendFrame(s, Pkt::kWrite, tx, static_cast<std::uint32_t>(data_off + data_len))) {
    return send_err;
  }
  FxStatus code = FxStatus::kFailure;
  const Error e = ReadStatus(s, rx, rx_cap, id, code);
  if ((Error::kOk != e) || (FxStatus::kOk != code)) {
    return Error::kWriteFailed;
  }
  return Error::kOk;
}

// Reads a STAT reply, which is either ATTRS (the normal case) or STATUS. A
// frame-level failure folds into kStatFailed so a truncated or oversized frame
// surfaces the same way as a missing attribute; every other malformation is a
// protocol error. The ATTRS-carrying counterpart to ReadStatus.
Error ReadAttrReply(ByteStream& s, std::uint8_t* rx, std::uint32_t rx_cap, std::uint32_t expect_id,
                    AttrsInfo& attrs) noexcept {
  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  if (Error::kOk != ReadFrame(s, rx, rx_cap, type, len)) {
    return Error::kStatFailed;
  }
  BufferReader r(rx, len);
  FxStatus code = FxStatus::kFailure;
  bool id_ok = (Error::kOk == ReadRequestId(r, expect_id));
  if (id_ok && (static_cast<std::uint8_t>(Pkt::kStatus) == type)) {
    id_ok = (Error::kOk == ParseStatusRest(r, code));
    if (!id_ok) {
      return Error::kSftpProtocolError;
    }
    return Error::kStatFailed;
  }
  if (!id_ok || (static_cast<std::uint8_t>(Pkt::kAttrs) != type)) {
    return Error::kSftpProtocolError;
  }
  return ParseAttrs(r, attrs) ? Error::kOk : Error::kStatFailed;
}

// Streams the local descriptor into WRITE frames until EOF and accumulates the
// byte count in `offset`. Each chunk is read straight into the frame's data
// region, so the prefix written by SendWriteChunk wraps bytes already present.
Error StreamWrites(ByteStream& s, std::uint8_t* tx, std::size_t tx_cap, std::uint8_t* rx, std::uint32_t rx_cap,
                   std::uint32_t& next_id, std::int32_t fd, const std::uint8_t* handle, std::uint32_t handle_len,
                   std::uint64_t& offset) noexcept {
  const std::uint32_t data_off = kWriteDataOffset(handle_len);
  offset = 0u;
  for (;;) {
    const std::int64_t n = ReadFull(fd, tx + data_off, kWriteChunkBytes);
    if (n < 0) {
      return Error::kWriteFailed;
    }
    if (0 == n) {
      break;
    }
    const std::uint32_t chunk = static_cast<std::uint32_t>(n);
    const std::uint32_t write_id = next_id;
    ++next_id;
    if ((data_off + chunk) > tx_cap) {
      return Error::kWriteFailed;
    }
    const Error e =
        SendWriteChunk(s, tx, tx_cap, write_id, handle, handle_len, offset, chunk, rx, rx_cap, Error::kWriteFailed);
    if (Error::kOk != e) {
      return e;
    }
    offset += chunk;
    if (chunk < kWriteChunkBytes) {
      break; /* short read means the descriptor hit EOF */
    }
  }
  return Error::kOk;
}

// Reads a NAME reply and returns its first entry. A STATUS reply is folded
// into `status_err` (the caller's operation-specific failure); any other
// malformed reply is a protocol error. Used by the single-name requests.
Error ReadNameReply(ByteStream& s, std::uint8_t* rx, std::uint32_t rx_cap, std::uint32_t expect_id, Error status_err,
                    const char*& name, std::uint32_t& name_len, AttrsInfo& attrs) noexcept {
  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  if (Error::kOk != ReadFrame(s, rx, rx_cap, type, len)) {
    return status_err;
  }
  BufferReader r(rx, len);
  if (Error::kOk != ReadRequestId(r, expect_id)) {
    return Error::kSftpProtocolError;
  }
  std::uint32_t count = 0u;
  const char* longname = nullptr;
  std::uint32_t longname_len = 0u;
  bool ok = true;
  if (static_cast<std::uint8_t>(Pkt::kStatus) == type) {
    FxStatus code = FxStatus::kFailure;
    ok = (Error::kOk == ParseStatusRest(r, code));
  } else if (static_cast<std::uint8_t>(Pkt::kName) == type) {
    ok = r.ReadU32(count) && (0u < count) && r.ReadString(name, name_len) && r.ReadString(longname, longname_len) &&
         ParseAttrs(r, attrs);
  } else {
    ok = false;
  }
  if (!ok) {
    return Error::kSftpProtocolError;
  }
  if (static_cast<std::uint8_t>(Pkt::kStatus) == type) {
    return status_err;
  }
  return Error::kOk;
}

// Sends STAT for `path` and reports whether it resolves. ATTRS and STATUS(OK)
// both mean "exists"; a frame-level failure folds into kMkdirFailed, a
// mismatched id is a protocol error, and any other STATUS means not found.
// This is the MKDIR-existing trap's disambiguation probe.
Error StatExists(ByteStream& s, std::uint8_t* tx, std::size_t tx_cap, std::uint8_t* rx, std::uint32_t rx_cap,
                 std::uint32_t& next_id, const char* path, bool& exists) noexcept {
  const std::uint32_t stat_id = next_id;
  ++next_id;
  BufferWriter sw(tx, tx_cap);
  (void)sw.WriteU32(stat_id);
  if (!sw.WriteCString(path, kMaxPathBytes) ||
      (Error::kOk != SendFrame(s, Pkt::kStat, tx, static_cast<std::uint32_t>(sw.size())))) {
    return Error::kMkdirFailed;
  }
  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  if (Error::kOk != ReadFrame(s, rx, rx_cap, type, len)) {
    return Error::kMkdirFailed;
  }
  exists = false;
  BufferReader sr(rx, len);
  if (Error::kOk != ReadRequestId(sr, stat_id)) {
    return Error::kSftpProtocolError;
  }
  if (static_cast<std::uint8_t>(Pkt::kAttrs) == type) {
    exists = true;
  } else if (static_cast<std::uint8_t>(Pkt::kStatus) == type) {
    FxStatus code = FxStatus::kFailure;
    if (Error::kOk != ParseStatusRest(sr, code)) {
      return Error::kSftpProtocolError;
    }
    exists = (FxStatus::kOk == code);
  }
  return Error::kOk;
}

// Sends one READ for `ask` bytes at `offset` and waits for the reply. A DATA
// reply points `data`/`data_len` into `rx`; a STATUS(EOF) reply sets `eof`; any
// other STATUS is kOpenFailed. Malformed replies are protocol errors, and a
// frame-level failure is returned raw because a broken channel must keep its
// own meaning. Scratch is reused per call; the data pointer is not copied.
Error ReadChunk(ByteStream& s, std::uint8_t* tx, std::size_t tx_cap, std::uint8_t* rx, std::uint32_t rx_cap,
                std::uint32_t& next_id, const std::uint8_t* handle, std::uint32_t handle_len, std::uint64_t offset,
                std::uint32_t ask, const char*& data, std::uint32_t& data_len, bool& eof) noexcept {
  const std::uint32_t read_id = next_id;
  ++next_id;
  data = nullptr;
  data_len = 0u;
  eof = false;
  BufferWriter w(tx, tx_cap);
  (void)w.WriteU32(read_id);
  (void)w.WriteString(reinterpret_cast<const char*>(handle), handle_len);
  (void)w.WriteU64(offset);
  (void)w.WriteU32(ask);
  Error err = w.ok() ? Error::kOk : Error::kSftpProtocolError;
  if (Error::kOk == err) {
    err = (Error::kOk == SendFrame(s, Pkt::kRead, tx, static_cast<std::uint32_t>(w.size())))
              ? Error::kOk
              : Error::kChannelWriteFailed;
  }
  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  if (Error::kOk == err) {
    err = ReadFrame(s, rx, rx_cap, type, len);
  }
  if (Error::kOk == err) {
    BufferReader r(rx, len);
    if (Error::kOk != ReadRequestId(r, read_id)) {
      err = Error::kSftpProtocolError;
    } else if (static_cast<std::uint8_t>(Pkt::kStatus) == type) {
      FxStatus code = FxStatus::kFailure;
      if (Error::kOk != ParseStatusRest(r, code)) {
        err = Error::kSftpProtocolError;
      } else if (FxStatus::kEof == code) {
        eof = true;
      } else {
        err = Error::kOpenFailed;
      }
    } else if (static_cast<std::uint8_t>(Pkt::kData) != type) {
      err = Error::kSftpProtocolError;
    } else if (!r.ReadString(data, data_len)) {
      err = Error::kSftpProtocolError;
    }
  }
  return err;
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
  const Error e = SendFrame(stream_, type, payload, len);
  if (Error::kOk != e) {
    return Status::error(e);
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
  bool wrote = w.WriteCString(path, kMaxPathBytes);
  if (wrote && (extra_len > 0u)) {
    wrote = w.WriteBytes(extra, extra_len);
  }
  Error err = (wrote && w.ok()) ? Error::kOk : Error::kOpenFailed;
  if (Error::kOk == err) {
    err = SendPacket(request, tx_, static_cast<std::uint32_t>(w.size())) ? Error::kOk : Error::kOpenFailed;
  }

  std::uint8_t type = 0u;
  std::uint32_t len = 0u;
  if (Error::kOk == err) {
    err = ReadFrame(stream_, rx_, sizeof(rx_), type, len);
    if (Error::kChannelReadFailed == err) {
      err = Error::kOpenFailed;
    }
  }
  bool missing_reply = false;
  if (Error::kOk == err) {
    BufferReader r(rx_, len);
    if (Error::kOk != ReadRequestId(r, id)) {
      err = Error::kSftpProtocolError;
    } else if (static_cast<std::uint8_t>(Pkt::kStatus) == type) {
      FxStatus code = FxStatus::kFailure;
      if (Error::kOk != ParseStatusRest(r, code)) {
        err = Error::kSftpProtocolError;
      } else if (FxStatus::kNoSuchFile == code) {
        missing_reply = true;
      } else {
        err = Error::kOpenFailed;
      }
    } else if (static_cast<std::uint8_t>(Pkt::kHandle) == type) {
      const char* h = nullptr;
      std::uint32_t h_len = 0u;
      if (!r.ReadString(h, h_len) || (0u == h_len) || (h_len > kMaxHandleBytes)) {
        err = Error::kOpenFailed;
      } else {
        (void)std::memcpy(handle, h, h_len);
        handle_len = h_len;
      }
    } else {
      err = Error::kSftpProtocolError;
    }
  }
  if (Error::kOk != err) {
    return Status::error(err);
  }
  missing = missing_reply;
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
  BufferReader r(rx_, len);
  std::uint32_t version = 0u;
  if ((static_cast<std::uint8_t>(Pkt::kVersion) != type) || !r.ReadU32(version) || (kProtocolVersion != version)) {
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

  const char* name = nullptr;
  std::uint32_t name_len = 0u;
  AttrsInfo attrs{};
  const Error e = ReadNameReply(stream_, rx_, sizeof(rx_), id, Error::kRealpathFailed, name, name_len, attrs);
  if ((Error::kOk != e) || (name_len > Path::capacity())) {
    /* Refuse to truncate a remote path silently. */
    return Result<Path>::error((Error::kOk != e) ? e : Error::kRealpathFailed);
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
  Error e = ReadStatus(stream_, rx_, sizeof(rx_), id, code);
  if (Error::kOk != e) {
    e = (Error::kChannelReadFailed == e) ? Error::kMkdirFailed : e;
  } else if (FxStatus::kOk != code) {
    if (FxStatus::kFailure == code) {
      /* OpenSSH answers FAILURE (not OK) when the directory already exists.
         Disambiguate with STAT: if the path resolves, the mkdir was a no-op.
         STAT succeeds as ATTRS (the normal case) or as STATUS on some servers. */
      bool exists = false;
      const Error se = StatExists(stream_, tx_, sizeof(tx_), rx_, sizeof(rx_), next_id_, path, exists);
      if (Error::kOk != se) {
        e = se;
      } else if (!exists) {
        e = Error::kMkdirFailed;
      }
    } else {
      e = Error::kMkdirFailed;
    }
  }
  if (Error::kOk != e) {
    return Status::error(e);
  }
  return Status::success();
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
  Error err = Error::kOk;
  StatBuf st{};
  if (LocalFstat(fd, &st) != 0) {
    err = Error::kOpenFailed;
  }
  const std::uint64_t local_size = static_cast<std::uint64_t>(st.st_size);

  std::uint8_t handle[kMaxHandleBytes];
  std::uint32_t handle_len = 0u;
  bool missing = false;
  if (Error::kOk == err) {
    /* OPEN(WRITE|CREAT|TRUNC) */
    std::uint8_t extras[8];
    PutBe32(extras, kFxWrite | kFxCreat | kFxTrunc);
    PutBe32(extras + 4u, 0u); /* empty ATTRS */
    if (!OpenHandle(Pkt::kOpen, remote_path, extras, sizeof(extras), handle, handle_len, missing)) {
      err = Error::kOpenFailed;
    }
  }

  /* WRITE chunks, streaming from the local fd directly into the frame. */
  std::uint64_t offset = 0u;
  const bool opened = (0u != handle_len);
  if ((Error::kOk == err) && opened) {
    err = StreamWrites(stream_, tx_, sizeof(tx_), rx_, sizeof(rx_), next_id_, fd, handle, handle_len, offset);
  }
  /* The remote handle is closed even when a WRITE failed. Gating the CLOSE on
     err left the handle open on the server for the rest of the session -- one
     per rejected upload -- and CLOSE is exactly the request that releases it. A
     close that itself fails only becomes the reported error when nothing worse
     already happened; the failed WRITE is the actionable one. */
  if (opened && !CloseHandle(handle, handle_len) && (Error::kOk == err)) {
    err = Error::kCloseFailed;
  }
  if (Error::kOk == err) {
    /* STAT + local/remote size comparison. No RENAME happens here. */
    const Result<std::uint64_t> remote_size = StatSize(remote_path);
    if (!remote_size) {
      err = Error::kStatFailed;
    } else if ((remote_size.value() != local_size) || (offset != local_size)) {
      err = Error::kSizeMismatch;
    }
  }
  /* Single cleanup point: every early exit joins here to close the fd. */
  (void)LocalClose(fd);
  if (Error::kOk != err) {
    return Status::error(err);
  }
  return Status::success();
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
  std::uint64_t offset = 0u;
  Error err = Error::kOk;
  while (Error::kOk == err) {
    const std::uint32_t remaining = capacity - size;
    /* When the buffer is full, ask for one byte so an oversized file is
       detected instead of silently truncated. */
    const std::uint32_t ask = (remaining == 0u) ? 1u : ((remaining < kReadChunkBytes) ? remaining : kReadChunkBytes);
    const char* data = nullptr;
    std::uint32_t data_len = 0u;
    bool eof = false;
    err = ReadChunk(stream_, tx_, sizeof(tx_), rx_, sizeof(rx_), next_id_, handle, handle_len, offset, ask, data,
                    data_len, eof);
    if (Error::kOk != err) {
      break;
    }
    if (eof) {
      break;
    }
    if (0u == data_len) {
      break; /* no progress: stop rather than spin */
    }
    if (data_len > (capacity - size)) {
      err = Error::kBufferTooSmall; /* over capacity: refuse loudly */
      break;
    }
    (void)std::memcpy(buffer + size, data, data_len);
    size += data_len;
    offset += data_len;
  }
  if (Error::kOk == err) {
    return CloseHandle(handle, handle_len);
  }
  (void)CloseHandle(handle, handle_len);
  return Status::error(err);
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

  const std::uint32_t data_off = kWriteDataOffset(handle_len);
  std::uint64_t offset = 0u;
  std::uint32_t pos = 0u;
  Error err = Error::kOk;
  while ((pos < length) && (Error::kOk == err)) {
    const std::uint32_t remaining = length - pos;
    const std::uint32_t chunk = (remaining < kWriteChunkBytes) ? remaining : kWriteChunkBytes;
    if ((data_off + chunk) > sizeof(tx_)) {
      err = Error::kWriteFailed;
      break;
    }
    (void)std::memcpy(tx_ + data_off, bytes + pos, chunk);
    err = SendWriteChunk(stream_, tx_, sizeof(tx_), next_id_++, handle, handle_len, offset, chunk, rx_, sizeof(rx_),
                         Error::kChannelWriteFailed);
    if (Error::kOk != err) {
      break;
    }
    pos += chunk;
    offset += chunk;
  }
  if (Error::kOk != err) {
    return fail(err);
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

  AttrsInfo attrs{};
  const Error e = ReadAttrReply(stream_, rx_, sizeof(rx_), id, attrs);
  if ((Error::kOk != e) || !attrs.has_size) {
    return Result<std::uint64_t>::error((Error::kOk != e) ? e : Error::kStatFailed);
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
  /* A cleanup that races another cleanup must not report failure: "already
     gone" is the desired end state. */
  if ((Error::kOk != e) || ((FxStatus::kOk != code) && (FxStatus::kNoSuchFile != code))) {
    return Status::error(Error::kRemoveFailed);
  }
  return Status::success();
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
  if ((Error::kOk != err) || !closed) {
    return Result<UploadListing>::error((Error::kOk != err) ? err : Error::kCloseFailed);
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

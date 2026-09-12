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
 * @file packet.hpp
 * @brief SFTP v3 wire codec.
 *
 * Fixed-capacity, zero-allocation encode/decode primitives plus the ATTRS
 * parser. Everything here is bounds-checked: a truncated or malformed packet
 * makes the reader report failure rather than reading out of bounds. The
 * ATTRS parser walks the SSH_FILEXFER_ATTRS bitmask instead of assuming a
 * fixed layout (verified against OpenSSH sftp-server: flags is not always 0xf).
 */
#pragma once

#include "picopaste/sftp/protocol.hpp"

#include <cstddef>
#include <cstdint>

namespace picopaste::sftp {

// 4-byte length prefix + 1-byte type.
inline constexpr std::uint32_t kFrameHeaderBytes = 5u;

// Longest SSH_FXP_HANDLE accepted from a server.
inline constexpr std::uint32_t kMaxHandleBytes = 128u;

/**
 * @brief Read a big-endian u32.
 * @param p At least 4 readable bytes; this does not bounds-check.
 */
std::uint32_t GetBe32(const std::uint8_t* p) noexcept;

/**
 * @brief Read a big-endian u64.
 * @param p At least 8 readable bytes; this does not bounds-check.
 */
std::uint64_t GetBe64(const std::uint8_t* p) noexcept;

/**
 * @brief Write `v` as a big-endian u32.
 * @param p At least 4 writable bytes; this does not bounds-check.
 */
void PutBe32(std::uint8_t* p, std::uint32_t v) noexcept;

/**
 * @brief Write `v` as a big-endian u64.
 * @param p At least 8 writable bytes; this does not bounds-check.
 */
void PutBe64(std::uint8_t* p, std::uint64_t v) noexcept;

// Writes big-endian fields into a caller-owned buffer. Once a write would
// exceed capacity the writer latches `ok_` to false and subsequent writes are
// no-ops; callers check ok() before sending.
class BufferWriter {
 public:
  BufferWriter(std::uint8_t* data, std::size_t capacity) noexcept : buf_(data), cap_(capacity), len_(0u), ok_(true) {}

  bool WriteU8(std::uint8_t v) noexcept;
  bool WriteU32(std::uint32_t v) noexcept;
  bool WriteU64(std::uint64_t v) noexcept;
  bool WriteBytes(const void* data, std::size_t len) noexcept;

  // Writes `len` raw bytes preceded by their u32 length. `len` must not
  // exceed the buffer; the string need not be NUL-terminated.
  bool WriteString(const char* s, std::uint32_t len) noexcept;

  // Writes a NUL-terminated string preceded by its length. Fails if no
  // terminator is found within `max_len` bytes (never truncates silently).
  bool WriteCString(const char* s, std::uint32_t max_len) noexcept;

  std::uint8_t* data() noexcept { return buf_; }
  std::size_t size() const noexcept { return len_; }
  bool ok() const noexcept { return ok_; }

 private:
  std::uint8_t* buf_;
  std::size_t cap_;
  std::size_t len_;
  bool ok_;
};

// Bounds-checked cursor over a received frame. Every read is validated; a
// short buffer makes the call return false and leaves the cursor unchanged.
class BufferReader {
 public:
  BufferReader(const std::uint8_t* data, std::size_t len) noexcept : buf_(data), len_(len), pos_(0u) {}

  bool ReadU8(std::uint8_t& out) noexcept;
  bool ReadU32(std::uint32_t& out) noexcept;
  bool ReadU64(std::uint64_t& out) noexcept;

  // Returns a pointer into the underlying buffer; no copy is made.
  bool ReadBytes(const std::uint8_t*& out, std::size_t len) noexcept;

  // Reads a u32 length then returns `len` bytes. The returned pointer is
  // valid only while the backing buffer is alive.
  bool ReadString(const char*& out, std::uint32_t& out_len) noexcept;

  std::size_t remaining() const noexcept { return len_ - pos_; }
  std::size_t pos() const noexcept { return pos_; }

 private:
  const std::uint8_t* buf_;
  std::size_t len_;
  std::size_t pos_;
};

// Decoded SSH_FILEXFER_ATTRS. Fields are only meaningful when their `has_*`
// flag is set; callers must not read absent fields.
struct AttrsInfo {
  std::uint64_t size = 0u;
  std::uint32_t uid = 0u;
  std::uint32_t gid = 0u;
  std::uint32_t perms = 0u;
  std::uint32_t atime = 0u;
  std::uint32_t mtime = 0u;
  bool has_size = false;
  bool has_uid_gid = false;
  bool has_perms = false;
  bool has_acmod_time = false;
};

// Parses an ATTRS structure by bitmask. Returns false on truncation, an
// unknown flag bit (which cannot be skipped safely), or a malformed extended
// section. Absent fields are left untouched.
bool ParseAttrs(BufferReader& r, AttrsInfo& out) noexcept;

}  // namespace picopaste::sftp

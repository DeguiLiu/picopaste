// picopaste — SFTP v3 wire codec implementation.
#include "packet.hpp"

#include <cstring>

namespace picopaste::sftp {

std::uint32_t GetBe32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24u) | (static_cast<std::uint32_t>(p[1]) << 16u) |
         (static_cast<std::uint32_t>(p[2]) << 8u) | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t GetBe64(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(GetBe32(p)) << 32u) | static_cast<std::uint64_t>(GetBe32(p + 4u));
}

void PutBe32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>((v >> 24u) & 0xffu);
  p[1] = static_cast<std::uint8_t>((v >> 16u) & 0xffu);
  p[2] = static_cast<std::uint8_t>((v >> 8u) & 0xffu);
  p[3] = static_cast<std::uint8_t>(v & 0xffu);
}

void PutBe64(std::uint8_t* p, std::uint64_t v) noexcept {
  PutBe32(p, static_cast<std::uint32_t>(v >> 32u));
  PutBe32(p + 4u, static_cast<std::uint32_t>(v & 0xffffffffu));
}

// ---------------------------------------------------------------------------
// BufferWriter
// ---------------------------------------------------------------------------

bool BufferWriter::WriteU8(std::uint8_t v) noexcept {
  if (!ok_ || (len_ + 1u) > cap_) {
    ok_ = false;
    return false;
  }
  buf_[len_] = v;
  ++len_;
  return true;
}

bool BufferWriter::WriteU32(std::uint32_t v) noexcept {
  if (!ok_ || (len_ + 4u) > cap_) {
    ok_ = false;
    return false;
  }
  PutBe32(buf_ + len_, v);
  len_ += 4u;
  return true;
}

bool BufferWriter::WriteU64(std::uint64_t v) noexcept {
  if (!ok_ || (len_ + 8u) > cap_) {
    ok_ = false;
    return false;
  }
  PutBe64(buf_ + len_, v);
  len_ += 8u;
  return true;
}

bool BufferWriter::WriteBytes(const void* data, std::size_t len) noexcept {
  if (!ok_ || (len_ + len) > cap_) {
    ok_ = false;
    return false;
  }
  if ((len > 0u) && (data != nullptr)) {
    (void)std::memcpy(buf_ + len_, data, len);
  }
  len_ += len;
  return true;
}

bool BufferWriter::WriteString(const char* s, std::uint32_t len) noexcept {
  if (!WriteU32(len)) {
    return false;
  }
  return WriteBytes(s, static_cast<std::size_t>(len));
}

bool BufferWriter::WriteCString(const char* s, std::uint32_t max_len) noexcept {
  if (s == nullptr) {
    ok_ = false;
    return false;
  }
  std::uint32_t n = 0u;
  while ((n < max_len) && (s[n] != '\0')) {
    ++n;
  }
  if (n == max_len) {
    /* No terminator within the caller's limit: refuse rather than truncate. */
    ok_ = false;
    return false;
  }
  return WriteString(s, n);
}

// ---------------------------------------------------------------------------
// BufferReader
// ---------------------------------------------------------------------------

bool BufferReader::ReadU8(std::uint8_t& out) noexcept {
  if ((pos_ + 1u) > len_) {
    return false;
  }
  out = buf_[pos_];
  ++pos_;
  return true;
}

bool BufferReader::ReadU32(std::uint32_t& out) noexcept {
  if ((pos_ + 4u) > len_) {
    return false;
  }
  out = GetBe32(buf_ + pos_);
  pos_ += 4u;
  return true;
}

bool BufferReader::ReadU64(std::uint64_t& out) noexcept {
  if ((pos_ + 8u) > len_) {
    return false;
  }
  out = GetBe64(buf_ + pos_);
  pos_ += 8u;
  return true;
}

bool BufferReader::ReadBytes(const std::uint8_t*& out, std::size_t len) noexcept {
  if ((pos_ + len) > len_) {
    return false;
  }
  out = buf_ + pos_;
  pos_ += len;
  return true;
}

bool BufferReader::ReadString(const char*& out, std::uint32_t& out_len) noexcept {
  std::uint32_t len = 0u;
  if (!ReadU32(len)) {
    return false;
  }
  const std::uint8_t* bytes = nullptr;
  if (!ReadBytes(bytes, static_cast<std::size_t>(len))) {
    return false;
  }
  out = reinterpret_cast<const char*>(bytes);
  out_len = len;
  return true;
}

// ---------------------------------------------------------------------------
// ATTRS
// ---------------------------------------------------------------------------

bool ParseAttrs(BufferReader& r, AttrsInfo& out) noexcept {
  std::uint32_t flags = 0u;
  if (!r.ReadU32(flags)) {
    return false;
  }

  if ((flags & kAttrSize) != 0u) {
    if (!r.ReadU64(out.size)) {
      return false;
    }
    out.has_size = true;
  }
  if ((flags & kAttrUidGid) != 0u) {
    if (!r.ReadU32(out.uid) || !r.ReadU32(out.gid)) {
      return false;
    }
    out.has_uid_gid = true;
  }
  if ((flags & kAttrPerms) != 0u) {
    if (!r.ReadU32(out.perms)) {
      return false;
    }
    out.has_perms = true;
  }
  if ((flags & kAttrAcmodTime) != 0u) {
    if (!r.ReadU32(out.atime) || !r.ReadU32(out.mtime)) {
      return false;
    }
    out.has_acmod_time = true;
  }
  if ((flags & kAttrExtended) != 0u) {
    std::uint32_t count = 0u;
    if (!r.ReadU32(count)) {
      return false;
    }
    for (std::uint32_t i = 0u; i < count; ++i) {
      const char* s = nullptr;
      std::uint32_t n = 0u;
      if (!r.ReadString(s, n) || !r.ReadString(s, n)) {
        return false;
      }
    }
  }

  const std::uint32_t known = kAttrSize | kAttrUidGid | kAttrPerms | kAttrAcmodTime | kAttrExtended;
  if ((flags & ~known) != 0u) {
    return false;
  }
  return true;
}

}  // namespace picopaste::sftp

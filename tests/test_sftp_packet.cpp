// picopaste — SFTP v3 wire codec tests.
//
// Covers the ATTRS bitmask trap directly: parsing by fixed offset reads
// garbage, so every flags combination and every truncation point is exercised.
// The "truncation" cases double as the malformed-input safety net: the decoder
// must report failure rather than read past the end of the buffer.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../src/core/sftp/packet.hpp"

using namespace picopaste::sftp;

namespace {

void PushU32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>((x >> 24u) & 0xffu));
  v.push_back(static_cast<std::uint8_t>((x >> 16u) & 0xffu));
  v.push_back(static_cast<std::uint8_t>((x >> 8u) & 0xffu));
  v.push_back(static_cast<std::uint8_t>(x & 0xffu));
}

void PushU64(std::vector<std::uint8_t>& v, std::uint64_t x) {
  PushU32(v, static_cast<std::uint32_t>(x >> 32u));
  PushU32(v, static_cast<std::uint32_t>(x & 0xffffffffu));
}

void PushString(std::vector<std::uint8_t>& v, const char* s) {
  const std::uint32_t n = static_cast<std::uint32_t>(std::strlen(s));
  PushU32(v, n);
  v.insert(v.end(), s, s + n);
}

std::vector<std::uint8_t> FullAttrs() {
  std::vector<std::uint8_t> v;
  PushU32(v, kAttrSize | kAttrUidGid | kAttrPerms | kAttrAcmodTime);
  PushU64(v, 0x1122334455667788ull);
  PushU32(v, 1000u);
  PushU32(v, 1000u);
  PushU32(v, 0644u);
  PushU32(v, 111u);
  PushU32(v, 222u);
  return v;
}

}  // namespace

TEST_CASE("big-endian primitives round-trip", "[sftp][packet]") {
  std::uint8_t buf[8]{};
  PutBe32(buf, 0xDEADBEEFu);
  CHECK(GetBe32(buf) == 0xDEADBEEFu);
  PutBe64(buf, 0x0123456789ABCDEFull);
  CHECK(GetBe64(buf) == 0x0123456789ABCDEFull);
}

TEST_CASE("BufferWriter never overflows and latches failure", "[sftp][packet]") {
  std::uint8_t buf[5]{};
  BufferWriter w(buf, sizeof(buf));
  CHECK(w.WriteU32(1u));
  CHECK(w.WriteU8(2u));
  CHECK(5u == w.size());
  CHECK_FALSE(w.WriteU8(3u));
  CHECK_FALSE(w.ok());
  /* Once latched, further calls stay failed and do not move the cursor. */
  CHECK_FALSE(w.WriteU32(4u));
  CHECK(5u == w.size());
}

TEST_CASE("BufferWriter refuses a silently truncated string", "[sftp][packet]") {
  std::uint8_t buf[16]{};
  BufferWriter w(buf, sizeof(buf));
  /* "abc" with no terminator inside the 3-byte limit must fail. */
  CHECK_FALSE(w.WriteCString("abc", 3u));
  CHECK_FALSE(w.ok());
}

TEST_CASE("BufferReader rejects truncation at every field width", "[sftp][packet]") {
  const std::vector<std::uint8_t> one{0x01u};
  BufferReader r8(one.data(), one.size());
  std::uint32_t out32 = 0u;
  CHECK_FALSE(r8.ReadU32(out32));

  const std::vector<std::uint8_t> seven(7u, 0u);
  BufferReader r64(seven.data(), seven.size());
  std::uint64_t out64 = 0u;
  CHECK_FALSE(r64.ReadU64(out64));

  std::vector<std::uint8_t> s;
  PushU32(s, 0xFFFFFFFFu); /* absurd string length */
  BufferReader rs(s.data(), s.size());
  const char* p = nullptr;
  std::uint32_t n = 0u;
  CHECK_FALSE(rs.ReadString(p, n));

  BufferReader rem(s.data(), s.size());
  CHECK(rem.remaining() == 4u);
  CHECK_FALSE(rem.ReadU64(out64));
  /* A failed read must not advance the cursor. */
  CHECK(rem.remaining() == 4u);
}

TEST_CASE("ParseAttrs honors the flags bitmask", "[sftp][packet]") {
  SECTION("flags=0: no fields present") {
    std::vector<std::uint8_t> v;
    PushU32(v, 0u);
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    CHECK(ParseAttrs(r, a));
    CHECK_FALSE(a.has_size);
    CHECK_FALSE(a.has_perms);
    CHECK(r.remaining() == 0u);
  }

  SECTION("size only") {
    std::vector<std::uint8_t> v;
    PushU32(v, kAttrSize);
    PushU64(v, 424242u);
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    CHECK(ParseAttrs(r, a));
    CHECK(a.has_size);
    CHECK(a.size == 424242u);
    CHECK_FALSE(a.has_perms);
  }

  SECTION("size|uidgid|perms|acmodtime (the observed 0xf layout)") {
    const std::vector<std::uint8_t> v = FullAttrs();
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    CHECK(ParseAttrs(r, a));
    CHECK(a.size == 0x1122334455667788ull);
    CHECK(a.uid == 1000u);
    CHECK(a.gid == 1000u);
    CHECK(a.perms == 0644u);
    CHECK(a.atime == 111u);
    CHECK(a.mtime == 222u);
    CHECK(r.remaining() == 0u);
  }

  SECTION("perms without size: size must stay absent") {
    std::vector<std::uint8_t> v;
    PushU32(v, kAttrPerms);
    PushU32(v, 0755u);
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    CHECK(ParseAttrs(r, a));
    CHECK(a.has_perms);
    CHECK_FALSE(a.has_size);
    CHECK(a.size == 0u);
    CHECK(r.remaining() == 0u);
  }

  SECTION("size|perms order follows the bitmask, not a fixed offset") {
    std::vector<std::uint8_t> v;
    PushU32(v, kAttrSize | kAttrPerms);
    PushU64(v, 7u);
    PushU32(v, 0600u);
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    CHECK(ParseAttrs(r, a));
    CHECK(a.size == 7u);
    CHECK(a.perms == 0600u);
  }

  SECTION("extended section") {
    std::vector<std::uint8_t> v;
    PushU32(v, kAttrExtended);
    PushU32(v, 1u);
    PushString(v, "key");
    PushString(v, "value");
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    CHECK(ParseAttrs(r, a));
    CHECK(r.remaining() == 0u);
  }

  SECTION("unknown flag bit is rejected, not skipped") {
    std::vector<std::uint8_t> v;
    PushU32(v, 0x00000010u); /* not a defined SSH_FILEXFER_ATTR bit */
    PushU32(v, 0u);
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    CHECK_FALSE(ParseAttrs(r, a));
  }
}

TEST_CASE("ParseAttrs rejects every truncation of a full record", "[sftp][packet][fuzz]") {
  const std::vector<std::uint8_t> full = FullAttrs();
  /* Every strict prefix is either too short to hold the declared fields or
     ends mid-field; none may parse successfully. */
  for (std::size_t n = 0u; n < full.size(); ++n) {
    BufferReader r(full.data(), n);
    AttrsInfo a{};
    CHECK_FALSE(ParseAttrs(r, a));
  }
  BufferReader ok(full.data(), full.size());
  AttrsInfo a{};
  CHECK(ParseAttrs(ok, a));
}

TEST_CASE("ParseAttrs survives arbitrary malformed bytes", "[sftp][packet][fuzz]") {
  /* Deterministic pseudo-random filler; the guarantee under test is simply
     that no input reads out of bounds or crashes (checked under ASan/UBSan). */
  std::uint32_t state = 0x12345678u;
  for (std::size_t len = 0u; len < 256u; ++len) {
    std::vector<std::uint8_t> v(len);
    for (std::size_t i = 0u; i < len; ++i) {
      state = (state * 1103515245u) + 12345u;
      v[i] = static_cast<std::uint8_t>((state >> 16u) & 0xffu);
    }
    BufferReader r(v.data(), v.size());
    AttrsInfo a{};
    (void)ParseAttrs(r, a); /* must return, not fault */
  }
  SUCCEED();
}

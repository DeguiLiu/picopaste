// picopaste — SFTP client tests.
//
// Two layers:
//   1. A scripted in-memory ByteStream drives the client through the three
//      empirically verified OpenSSH traps (MKDIR-existing, RENAME-existing,
//      ATTRS bitmask) and through malformed responses.
//   2. A real end-to-end test against this machine's own OpenSSH sftp-server
//      over `ssh -s localhost sftp`, exercising multi-chunk streaming,
//      size verification and atomic rename. It skips (not fails) when the
//      local sftp subsystem is unavailable.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "picopaste/sftp/client.hpp"
#include "picopaste/sftp/protocol.hpp"
#include "../src/platform/posix/stream_posix.hpp"

using namespace picopaste;
using namespace picopaste::sftp;

namespace {

// ---------------------------------------------------------------------------
// Scripted ByteStream
// ---------------------------------------------------------------------------

struct FakePipe {
  std::vector<std::uint8_t> in;   /* bytes the client will read */
  std::size_t in_pos = 0u;
  std::vector<std::uint8_t> out;  /* bytes the client wrote */
};

bool FakeWrite(void* ctx, const std::uint8_t* data, std::size_t len) noexcept {
  auto* p = static_cast<FakePipe*>(ctx);
  p->out.insert(p->out.end(), data, data + len);
  return true;
}

bool FakeRead(void* ctx, std::uint8_t* dst, std::size_t len) noexcept {
  auto* p = static_cast<FakePipe*>(ctx);
  if ((p->in_pos + len) > p->in.size()) {
    return false;
  }
  (void)std::memcpy(dst, p->in.data() + p->in_pos, len);
  p->in_pos += len;
  return true;
}

void FakeClose(void* ctx) noexcept { (void)ctx; }

ByteStream MakeFake(FakePipe& p) {
  ByteStream b{};
  b.ctx = &p;
  b.write = &FakeWrite;
  b.read = &FakeRead;
  b.close = &FakeClose;
  return b;
}

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

void AppendFrame(FakePipe& p, Pkt type, const std::vector<std::uint8_t>& payload) {
  PushU32(p.in, static_cast<std::uint32_t>(payload.size()) + 1u);
  p.in.push_back(static_cast<std::uint8_t>(type));
  p.in.insert(p.in.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> StatusPayload(std::uint32_t id, FxStatus code) {
  std::vector<std::uint8_t> v;
  PushU32(v, id);
  PushU32(v, static_cast<std::uint32_t>(code));
  PushString(v, "message");
  PushString(v, "en");
  return v;
}

// Appends a successful VERSION handshake so the client reaches initialized().
void AppendVersionOk(FakePipe& p) {
  std::vector<std::uint8_t> v;
  PushU32(v, kProtocolVersion);
  AppendFrame(p, Pkt::kVersion, v);
}

// ---------------------------------------------------------------------------
// Integration plumbing
// ---------------------------------------------------------------------------

struct StreamGuard {
  ByteStream b{};
  ~StreamGuard() {
    if (b.valid()) {
      b.close(b.ctx);
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// INIT
// ---------------------------------------------------------------------------

TEST_CASE("Init accepts a v3 VERSION handshake", "[sftp][client]") {
  FakePipe p;
  AppendVersionOk(p);
  Client c(MakeFake(p));
  CHECK(c.Init());
  CHECK(c.initialized());
}

TEST_CASE("Init fails on a non-v3 peer", "[sftp][client]") {
  SECTION("wrong version") {
    FakePipe p;
    std::vector<std::uint8_t> v;
    PushU32(v, 2u);
    AppendFrame(p, Pkt::kVersion, v);
    Client c(MakeFake(p));
    REQUIRE_FALSE(c.Init());
    CHECK(c.Init().get_error() == Error::kSftpInitFailed);
  }
  SECTION("wrong packet type") {
    FakePipe p;
    AppendFrame(p, Pkt::kStatus, StatusPayload(0u, FxStatus::kOk));
    Client c(MakeFake(p));
    REQUIRE_FALSE(c.Init());
  }
  SECTION("truncated / EOF (no sftp subsystem)") {
    FakePipe p;
    Client c(MakeFake(p));
    REQUIRE_FALSE(c.Init());
    CHECK(c.Init().get_error() == Error::kSftpInitFailed);
  }
}

// ---------------------------------------------------------------------------
// REALPATH / STAT
// ---------------------------------------------------------------------------

TEST_CASE("Realpath returns the NAME payload", "[sftp][client]") {
  FakePipe p;
  AppendVersionOk(p);
  std::vector<std::uint8_t> name;
  PushU32(name, 1u); /* id */
  PushU32(name, 1u); /* count */
  PushString(name, "/home/tester");
  PushString(name, "/home/tester");
  PushU32(name, 0u); /* empty attrs */
  AppendFrame(p, Pkt::kName, name);

  Client c(MakeFake(p));
  REQUIRE(c.Init());
  const auto r = c.Realpath(".");
  REQUIRE(r.has_value());
  CHECK(std::string(r.value().c_str()) == "/home/tester");
}

TEST_CASE("Realpath maps errors and malformed NAME to protocol/realpath errors", "[sftp][client]") {
  SECTION("STATUS reply") {
    FakePipe p;
    AppendVersionOk(p);
    AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kNoSuchFile));
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    const auto r = c.Realpath(".");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.get_error() == Error::kRealpathFailed);
  }
  SECTION("truncated NAME payload") {
    FakePipe p;
    AppendVersionOk(p);
    std::vector<std::uint8_t> name;
    PushU32(name, 1u);
    PushU32(name, 1u);
    PushU32(name, 100u); /* string length far past the end */
    AppendFrame(p, Pkt::kName, name);
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    const auto r = c.Realpath(".");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.get_error() == Error::kSftpProtocolError);
  }
}

TEST_CASE("StatSize parses ATTRS by bitmask", "[sftp][client]") {
  SECTION("size present") {
    FakePipe p;
    AppendVersionOk(p);
    std::vector<std::uint8_t> a;
    PushU32(a, 1u);
    PushU32(a, kAttrSize);
    PushU64(a, 987654321u);
    AppendFrame(p, Pkt::kAttrs, a);
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    const auto r = c.StatSize("/x");
    REQUIRE(r.has_value());
    CHECK(r.value() == 987654321u);
  }
  SECTION("no size bit: kStatFailed, never garbage") {
    FakePipe p;
    AppendVersionOk(p);
    std::vector<std::uint8_t> a;
    PushU32(a, 1u);
    PushU32(a, kAttrPerms);
    PushU32(a, 0644u);
    AppendFrame(p, Pkt::kAttrs, a);
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    const auto r = c.StatSize("/x");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.get_error() == Error::kStatFailed);
  }
  SECTION("unexpected packet type") {
    FakePipe p;
    AppendVersionOk(p);
    std::vector<std::uint8_t> a;
    PushU32(a, 1u);
    PushU32(a, 0u);
    AppendFrame(p, Pkt::kData, a);
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    const auto r = c.StatSize("/x");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.get_error() == Error::kSftpProtocolError);
  }
}

TEST_CASE("Malformed frame length is rejected as a protocol error", "[sftp][client][fuzz]") {
  FakePipe p;
  AppendVersionOk(p);
  /* Oversized length prefix: the client must refuse to buffer it. */
  PushU32(p.in, 0x7FFFFFFFu);
  p.in.push_back(static_cast<std::uint8_t>(Pkt::kAttrs));
  Client c(MakeFake(p));
  REQUIRE(c.Init());
  const auto r = c.StatSize("/x");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.get_error() == Error::kStatFailed); /* mapped from protocol error */
}

// ---------------------------------------------------------------------------
// The three traps
// ---------------------------------------------------------------------------

TEST_CASE("Mkdir maps FAILURE-on-existing to success", "[sftp][client][trap]") {
  SECTION("existing directory: MKDIR FAILURE then STAT OK") {
    FakePipe p;
    AppendVersionOk(p);
    AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kFailure));
    AppendFrame(p, Pkt::kStatus, StatusPayload(2u, FxStatus::kOk));
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    CHECK(c.Mkdir("/home/tester/.cache"));
  }
  SECTION("existing directory: STAT answers with ATTRS (real OpenSSH)") {
    FakePipe p;
    AppendVersionOk(p);
    AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kFailure));
    std::vector<std::uint8_t> attrs;
    PushU32(attrs, 2u); /* id */
    PushU32(attrs, 0u); /* empty ATTRS flags */
    AppendFrame(p, Pkt::kAttrs, attrs);
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    CHECK(c.Mkdir("/home/tester/.cache"));
  }
  SECTION("missing parent: MKDIR FAILURE then STAT NO_SUCH_FILE") {
    FakePipe p;
    AppendVersionOk(p);
    AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kFailure));
    AppendFrame(p, Pkt::kStatus, StatusPayload(2u, FxStatus::kNoSuchFile));
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    const Status s = c.Mkdir("/home/tester/nope/child");
    REQUIRE_FALSE(s);
    CHECK(s.get_error() == Error::kMkdirFailed);
  }
  SECTION("fresh directory: MKDIR OK") {
    FakePipe p;
    AppendVersionOk(p);
    AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kOk));
    Client c(MakeFake(p));
    REQUIRE(c.Init());
    CHECK(c.Mkdir("/home/tester/.cache"));
  }
}

TEST_CASE("MkdirAll tolerates failure at each level", "[sftp][client][trap]") {
  FakePipe p;
  AppendVersionOk(p);
  /* /a -> created; /a/b -> already exists (FAILURE + STAT OK);
     /a/b/c -> created. */
  AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kOk));
  AppendFrame(p, Pkt::kStatus, StatusPayload(2u, FxStatus::kFailure));
  AppendFrame(p, Pkt::kStatus, StatusPayload(3u, FxStatus::kOk));
  AppendFrame(p, Pkt::kStatus, StatusPayload(4u, FxStatus::kOk));
  Client c(MakeFake(p));
  REQUIRE(c.Init());
  CHECK(c.MkdirAll("/a/b/c"));
}

TEST_CASE("Rename onto an existing target fails", "[sftp][client][trap]") {
  FakePipe p;
  AppendVersionOk(p);
  AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kFailure));
  Client c(MakeFake(p));
  REQUIRE(c.Init());
  const Status s = c.Rename("/a/tmp", "/a/final");
  REQUIRE_FALSE(s);
  CHECK(s.get_error() == Error::kRenameFailed);
}

TEST_CASE("Remove reports non-OK status", "[sftp][client]") {
  FakePipe p;
  AppendVersionOk(p);
  AppendFrame(p, Pkt::kStatus, StatusPayload(1u, FxStatus::kNoSuchFile));
  Client c(MakeFake(p));
  REQUIRE(c.Init());
  REQUIRE_FALSE(c.Remove("/a/nope"));
}

// ---------------------------------------------------------------------------
// Real end-to-end against the machine's own sftp-server
// ---------------------------------------------------------------------------

TEST_CASE("end-to-end upload against local OpenSSH sftp-server", "[sftp][client][integration]") {
  const char* argv[] = {"ssh",
                        "-o",
                        "ClearAllForwardings=yes",
                        "-o",
                        "BatchMode=yes",
                        "-o",
                        "LogLevel=ERROR",
                        "-s",
                        "localhost",
                        "sftp",
                        nullptr};
  auto spawned = posix::SpawnStream(argv);
  if (!spawned.has_value()) {
    SKIP("could not spawn ssh: local sftp subsystem unavailable");
  }

  StreamGuard guard{spawned.value()};
  Client c(guard.b);
  if (!c.Init()) {
    SKIP("local sftp subsystem did not answer v3");
  }

  const auto home = c.Realpath(".");
  REQUIRE(home.has_value());

  const std::string base(home.value().c_str());
  const std::string pid = std::to_string(static_cast<long>(::getpid()));
  const std::string dir = base + "/.cache/picopaste-test-" + pid;
  const Status mk = c.MkdirAll(dir.c_str());
  REQUIRE(mk);

  const std::string remote = dir + "/upload-" + pid + ".bin";
  const std::string renamed = dir + "/upload-" + pid + "-renamed.bin";

  /* Local file > 2x the write chunk so chunking is genuinely exercised. */
  char local_tmpl[] = "/tmp/picopaste-sftp-upload-XXXXXX";
  const int lfd = ::mkstemp(local_tmpl);
  REQUIRE(lfd >= 0);
  std::vector<std::uint8_t> data((2u * kWriteChunkBytes) + 12345u);
  for (std::size_t i = 0u; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xffu);
  }
  {
    std::size_t off = 0u;
    while (off < data.size()) {
      const ssize_t n = ::write(lfd, data.data() + off, data.size() - off);
      REQUIRE(n > 0);
      off += static_cast<std::size_t>(n);
    }
  }
  (void)::close(lfd);

  const Status up = c.UploadFile(remote.c_str(), local_tmpl);
  if (!up) {
    UNSCOPED_INFO("UploadFile error=" << static_cast<int>(up.get_error()));
  }
  REQUIRE(up);

  const auto stat1 = c.StatSize(remote.c_str());
  REQUIRE(stat1.has_value());
  CHECK(stat1.value() == data.size());

  REQUIRE(c.Rename(remote.c_str(), renamed.c_str()));
  const auto stat2 = c.StatSize(renamed.c_str());
  REQUIRE(stat2.has_value());
  CHECK(stat2.value() == data.size());

  REQUIRE(c.Remove(renamed.c_str()));
  (void)::unlink(local_tmpl);

  std::printf("[integration] realpath=%s bytes=%llu renamed=%s\n", home.value().c_str(),
              static_cast<unsigned long long>(stat2.value()), renamed.c_str());
  SUCCEED("end-to-end upload, size verification and rename executed");
}

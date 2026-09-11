// picopaste — upload-pipeline tests.
//
// The pipeline is the one place where the product's central promise is either
// kept or broken: on any failure before the clipboard is published, NOTHING may
// be written to the clipboard and NO keystroke may be sent. These tests drive
// the real pipeline over a REAL `ssh -s localhost sftp` channel (the same
// protocol the Windows client speaks) with a fake clipboard and a fake injector,
// so the upload genuinely happens while the two side effects are observed.
//
// Coverage:
//   * happy path: capture -> mkdir -> upload -> size verify -> rename ->
//     set clipboard text -> inject -> restore -> release, asserted end to end.
//   * single-flight: a second trigger while one is running returns kBusy and
//     performs no side effect.
//   * failure paths: a capture failure, a directory failure, and a remote size
//     mismatch each assert set_text_calls == 0 and paste_calls == 0.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

#include "../src/core/app/upload_pipeline.hpp"
#include "picopaste/config.hpp"
#include "picopaste/sftp/client.hpp"

using namespace picopaste;
using namespace picopaste::sftp;

namespace {

// ---------------------------------------------------------------------------
// Fake clipboard / injector
// ---------------------------------------------------------------------------

// Backing state for the fake function tables. Counters are atomic because the
// single-flight test runs one trigger on a worker thread while the main thread
// observes that no side effect happened.
struct FakeOps {
  std::atomic<int> capture_calls{0};
  std::atomic<int> set_text_calls{0};
  std::atomic<int> paste_calls{0};
  std::atomic<int> restore_calls{0};
  std::atomic<int> release_calls{0};
  std::atomic<std::uint32_t> last_delay{0};

  Error capture_error = Error::kOk;
  Error set_text_error = Error::kOk;
  Error paste_error = Error::kOk;

  // Local temp file the fake "capture" produced (written by the test).
  char local_path[kLocalPathBytes] = {};
  std::uint64_t bytes = 0;
  // Reported byte count differs from the real file by this much: how the size
  // verification failure is forced against a real server.
  std::int32_t bytes_delta = 0;

  char clipboard_text[kLocalPathBytes] = {};

  // Gate used by the single-flight test to hold the first run inside capture.
  std::mutex gate_mutex;
  std::condition_variable gate_cv;
  bool gate_entered = false;
  bool gate_open = true;

  void Wire(ClipboardOps& clip, InjectOps& inject) noexcept {
    clip.ctx = this;
    clip.capture = &FakeCapture;
    clip.set_text = &FakeSetText;
    clip.restore = &FakeRestore;
    clip.release = &FakeRelease;
    inject.ctx = this;
    inject.paste = &FakePaste;
  }

  static Error FakeCapture(void* ctx, const Config& cfg, CapturedClip& out) noexcept {
    (void)cfg;
    FakeOps* self = static_cast<FakeOps*>(ctx);
    self->capture_calls.fetch_add(1, std::memory_order_relaxed);
    if (Error::kOk != self->capture_error) {
      return self->capture_error;
    }
    if ('\0' == self->local_path[0]) {
      return Error::kTempFileFailed;
    }
    {
      std::unique_lock<std::mutex> lock(self->gate_mutex);
      self->gate_entered = true;
      self->gate_cv.notify_all();
      self->gate_cv.wait(lock, [self] { return self->gate_open; });
    }
    out.local_path.assign(osp::TruncateToCapacity, self->local_path);
    const std::int64_t reported = static_cast<std::int64_t>(self->bytes) + self->bytes_delta;
    out.bytes = static_cast<std::uint64_t>(reported);
    out.restore_token = self;
    out.captured = true;
    return Error::kOk;
  }

  static Error FakeSetText(void* ctx, const char* utf8) noexcept {
    FakeOps* self = static_cast<FakeOps*>(ctx);
    self->set_text_calls.fetch_add(1, std::memory_order_relaxed);
    if (Error::kOk != self->set_text_error) {
      return self->set_text_error;
    }
    (void)std::snprintf(self->clipboard_text, sizeof(self->clipboard_text), "%s",
                        (utf8 != nullptr) ? utf8 : "");
    return Error::kOk;
  }

  static Error FakeRestore(void* ctx, void* token) noexcept {
    (void)token;
    static_cast<FakeOps*>(ctx)->restore_calls.fetch_add(1, std::memory_order_relaxed);
    return Error::kOk;
  }

  static void FakeRelease(void* ctx, void* token) noexcept {
    (void)token;
    static_cast<FakeOps*>(ctx)->release_calls.fetch_add(1, std::memory_order_relaxed);
  }

  static Error FakePaste(void* ctx, std::uint32_t delay_ms) noexcept {
    FakeOps* self = static_cast<FakeOps*>(ctx);
    self->paste_calls.fetch_add(1, std::memory_order_relaxed);
    self->last_delay.store(delay_ms, std::memory_order_relaxed);
    return self->paste_error;
  }
};

// ---------------------------------------------------------------------------
// Integration plumbing
// ---------------------------------------------------------------------------

struct Session {
  ByteStream b{};
  Client client;

  explicit Session(ByteStream stream) noexcept : b(stream), client(b) {}
  ~Session() {
    if (b.valid()) {
      b.close(b.ctx);
    }
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
};

std::unique_ptr<Session> ConnectSftp() {
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
  auto spawned = test::SpawnStream(argv);
  if (!spawned.has_value()) {
    return nullptr;
  }
  auto session = std::make_unique<Session>(spawned.value());
  if (!session->client.Init()) {
    return nullptr;
  }
  return session;
}

std::string CacheDir(const char* leaf) {
  const std::string pid = std::to_string(test::ProcessId());
  return std::string("/.cache/") + leaf + "-" + pid;
}

const char* BaseName(const char* path) {
  const char* slash = std::strrchr(path, '/');
  return (nullptr == slash) ? path : (slash + 1);
}

// A local temp file of `len` deterministic bytes.
test::TempFile MakeLocalFile(std::size_t len) {
  std::vector<std::uint8_t> data(len);
  for (std::size_t i = 0u; i < len; ++i) {
    data[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xffu);
  }
  return test::TempFile::Create(data.data(), data.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_CASE("pipeline uploads, verifies, publishes the path and injects exactly once",
          "[pipeline][integration]") {
  auto session = ConnectSftp();
  if (session == nullptr) {
    SKIP("local sftp subsystem unavailable");
  }
  Client& client = session->client;

  const auto home = client.Realpath(".");
  REQUIRE(home.has_value());
  const std::string rel = CacheDir("picopaste-pipe");
  const std::string abs = std::string(home.value().c_str()) + rel;

  Config cfg = DefaultConfig();
  cfg.remote_dir.assign(osp::TruncateToCapacity, (std::string("~") + rel).c_str());
  cfg.restore_clipboard = true;
  cfg.delay_ms = 0;

  // Larger than one 64 KB write chunk so chunked streaming is exercised.
  const std::size_t len = (2u * kWriteChunkBytes) + 4321u;
  const test::TempFile local = MakeLocalFile(len);
  REQUIRE(local.valid());

  FakeOps ops;
  (void)std::snprintf(ops.local_path, sizeof(ops.local_path), "%s", local.path());
  ops.bytes = len;
  ClipboardOps clip{};
  InjectOps inject{};
  ops.Wire(clip, inject);

  UploadPipeline pipeline(cfg);
  // Real wall clock: the retention age rule compares against the remote mtime,
  // so a fixed future "now" would legitimately prune the file we just uploaded.
  const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
  const auto result = pipeline.Run(client, clip, inject, now);

  REQUIRE(result.has_value());
  const UploadReport& report = result.value();

  // The remote name is ours and prunable by the retention pattern.
  CHECK(MatchesUploadName(BaseName(report.remote_path.c_str()),
                          static_cast<std::uint32_t>(std::strlen(BaseName(report.remote_path.c_str())))));
  CHECK(std::strncmp(report.remote_path.c_str(), abs.c_str(), abs.size()) == 0);
  CHECK(report.bytes == len);

  // Remote file really exists with the captured size.
  const auto size = client.StatSize(report.remote_path.c_str());
  REQUIRE(size.has_value());
  CHECK(size.value() == len);
  CHECK(report.pruned);
  CHECK_FALSE(report.prune_failed);

  // Exactly one capture, one clipboard write, one chord, one restore, one release.
  CHECK(ops.capture_calls.load() == 1);
  CHECK(ops.set_text_calls.load() == 1);
  CHECK(ops.paste_calls.load() == 1);
  CHECK(ops.restore_calls.load() == 1);
  CHECK(ops.release_calls.load() == 1);
  CHECK(report.clipboard_restored);
  CHECK(std::strcmp(ops.clipboard_text, report.remote_path.c_str()) == 0);

  // Retention kept the single upload: the final name is the only match.
  const auto listing = client.ListDir(abs.c_str());
  REQUIRE(listing.has_value());
  CHECK(listing.value().entries.size() == 1u);

  REQUIRE(client.Remove(report.remote_path.c_str()));
  std::printf("[pipeline] remote=%s bytes=%llu pruned=%u\n", report.remote_path.c_str(),
              static_cast<unsigned long long>(report.bytes),
              static_cast<unsigned>(report.pruned_files));
}

// ---------------------------------------------------------------------------
// Single-flight
// ---------------------------------------------------------------------------

TEST_CASE("a second trigger while one is running returns kBusy and acts on nothing",
          "[pipeline]") {
  auto session = ConnectSftp();
  if (session == nullptr) {
    SKIP("local sftp subsystem unavailable");
  }
  Client& client = session->client;

  const test::TempFile local = MakeLocalFile(4096u);
  REQUIRE(local.valid());

  Config cfg = DefaultConfig();
  cfg.remote_dir.assign(osp::TruncateToCapacity, (std::string("~") + CacheDir("picopaste-pipe-busy")).c_str());
  cfg.delay_ms = 0;

  FakeOps ops;
  (void)std::snprintf(ops.local_path, sizeof(ops.local_path), "%s", local.path());
  ops.bytes = 4096u;
  ClipboardOps clip{};
  InjectOps inject{};
  ops.Wire(clip, inject);

  UploadPipeline pipeline(cfg);

  // Hold the first run inside capture; the gate is opened by the main thread.
  {
    std::unique_lock<std::mutex> lock(ops.gate_mutex);
    ops.gate_open = false;
    ops.gate_entered = false;
  }

  Result<UploadReport> first = Result<UploadReport>::error(Error::kOk);
  std::thread worker([&pipeline, &client, &clip, &inject, &first] {
    first = pipeline.Run(client, clip, inject, 1900000001ull);
  });

  {
    std::unique_lock<std::mutex> lock(ops.gate_mutex);
    ops.gate_cv.wait(lock, [&ops] { return ops.gate_entered; });
  }
  CHECK(pipeline.in_flight());

  const auto second = pipeline.Run(client, clip, inject, 1900000002ull);
  REQUIRE_FALSE(second.has_value());
  CHECK(second.get_error() == Error::kBusy);
  // The refused trigger must not have touched the clipboard or typed anything.
  CHECK(ops.set_text_calls.load() == 0);
  CHECK(ops.paste_calls.load() == 0);

  {
    std::unique_lock<std::mutex> lock(ops.gate_mutex);
    ops.gate_open = true;
    ops.gate_cv.notify_all();
  }
  worker.join();

  REQUIRE(first.has_value());
  CHECK_FALSE(pipeline.in_flight());
  REQUIRE(client.Remove(first.value().remote_path.c_str()));
  std::printf("[pipeline] busy-refused second error=%u\n",
              static_cast<unsigned>(second.get_error()));
}

// ---------------------------------------------------------------------------
// Failure paths: no clipboard write, no keystroke
// ---------------------------------------------------------------------------

TEST_CASE("a failed remote size verification writes nothing to the clipboard",
          "[pipeline][integration]") {
  auto session = ConnectSftp();
  if (session == nullptr) {
    SKIP("local sftp subsystem unavailable");
  }
  Client& client = session->client;

  const auto home = client.Realpath(".");
  REQUIRE(home.has_value());
  const std::string rel = CacheDir("picopaste-pipe-mismatch");
  const std::string abs = std::string(home.value().c_str()) + rel;

  Config cfg = DefaultConfig();
  cfg.remote_dir.assign(osp::TruncateToCapacity, (std::string("~") + rel).c_str());
  cfg.restore_clipboard = true;
  cfg.delay_ms = 0;

  const test::TempFile local = MakeLocalFile(8192u);
  REQUIRE(local.valid());

  FakeOps ops;
  (void)std::snprintf(ops.local_path, sizeof(ops.local_path), "%s", local.path());
  ops.bytes = 8192u;
  ops.bytes_delta = 1;  // server will report 8192; we claim 8193
  ClipboardOps clip{};
  InjectOps inject{};
  ops.Wire(clip, inject);

  UploadPipeline pipeline(cfg);
  const auto result = pipeline.Run(client, clip, inject, 1900000003ull);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.get_error() == Error::kSizeMismatch);
  CHECK(ops.capture_calls.load() == 1);
  CHECK(ops.set_text_calls.load() == 0);
  CHECK(ops.paste_calls.load() == 0);
  CHECK(ops.restore_calls.load() == 0);
  // The temp was removed on the remote: nothing half-published survives.
  const auto listing = client.ListDir(abs.c_str());
  REQUIRE(listing.has_value());
  CHECK(listing.value().entries.size() == 0u);
  CHECK('\0' == ops.clipboard_text[0]);
}

TEST_CASE("a directory failure writes nothing to the clipboard and sends no chord",
          "[pipeline][integration]") {
  auto session = ConnectSftp();
  if (session == nullptr) {
    SKIP("local sftp subsystem unavailable");
  }
  Client& client = session->client;

  Config cfg = DefaultConfig();
  // /proc exists and cannot contain a new directory: MkdirAll fails after the
  // first successful level, before any upload is attempted.
  cfg.remote_dir.assign(osp::TruncateToCapacity, "/proc/picopaste-nope/uploads");
  cfg.delay_ms = 0;

  const test::TempFile local = MakeLocalFile(256u);
  REQUIRE(local.valid());

  FakeOps ops;
  (void)std::snprintf(ops.local_path, sizeof(ops.local_path), "%s", local.path());
  ops.bytes = 256u;
  ClipboardOps clip{};
  InjectOps inject{};
  ops.Wire(clip, inject);

  UploadPipeline pipeline(cfg);
  const auto result = pipeline.Run(client, clip, inject, 1900000004ull);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.get_error() == Error::kMkdirFailed);
  CHECK(ops.set_text_calls.load() == 0);
  CHECK(ops.paste_calls.load() == 0);
  CHECK('\0' == ops.clipboard_text[0]);
}

TEST_CASE("no image on the clipboard writes nothing and sends no chord", "[pipeline]") {
  // No channel is needed: capture fails before the client is ever touched.
  Config cfg = DefaultConfig();
  cfg.remote_dir.assign(osp::TruncateToCapacity, "~/.cache/picopaste-pipe-noimg");
  cfg.delay_ms = 0;

  FakeOps ops;
  ops.capture_error = Error::kNoImageInClipboard;
  ClipboardOps clip{};
  InjectOps inject{};
  ops.Wire(clip, inject);

  // A real channel is still required by the interface; the client is never
  // reached because capture fails first.
  auto session = ConnectSftp();
  if (session == nullptr) {
    SKIP("local sftp subsystem unavailable");
  }
  UploadPipeline pipeline(cfg);
  const auto result = pipeline.Run(session->client, clip, inject, 1900000005ull);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.get_error() == Error::kNoImageInClipboard);
  CHECK(ops.set_text_calls.load() == 0);
  CHECK(ops.paste_calls.load() == 0);
  CHECK('\0' == ops.clipboard_text[0]);
}

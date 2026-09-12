// picopaste — log ring and bounded log sink tests.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include "../src/core/log/log_ring.hpp"
#include "../src/core/log/log_sink.hpp"
#include "picopaste/memsample.hpp"

namespace {

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const char* tag) {
    path = std::filesystem::temp_directory_path() / (std::string("picopaste_log_") + tag);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::string file(const char* name) const { return (path / name).string(); }
};

std::uintmax_t FileSize(const std::string& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0U : size;
}

}  // namespace

TEST_CASE("LogRing round-trips a record", "[log]") {
  picopaste::LogRing ring;
  CHECK(ring.Empty());
  CHECK(ring.Size() == 0U);

  const picopaste::LogRecord in = picopaste::MakeLogRecord(picopaste::LogLevel::kWarn, "hello %d", 7);
  CHECK(in.level == picopaste::LogLevel::kWarn);
  CHECK(std::string(in.message) == "hello 7");
  REQUIRE(ring.TryPush(picopaste::LogProducer::kMain, in));
  CHECK(ring.Size() == 1U);
  CHECK(ring.Size(picopaste::LogProducer::kMain) == 1U);
  CHECK(ring.Accepted() == 1U);
  CHECK(ring.Dropped() == 0U);

  picopaste::LogRecord out{};
  REQUIRE(ring.TryPop(out));
  CHECK(std::string(out.message) == "hello 7");
  CHECK(out.level == picopaste::LogLevel::kWarn);
  CHECK(out.wallclock_ms < 1000U);
  CHECK(out.monotonic_us > 0U);
  CHECK(ring.Empty());
}

TEST_CASE("LogRing is bounded and counts drops instead of blocking", "[log]") {
  picopaste::LogRing ring;
  for (std::uint32_t i = 0; i < picopaste::LogRing::Depth(); ++i) {
    REQUIRE(ring.TryPush(picopaste::LogProducer::kMain,
                         picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "record %u", i)));
  }
  CHECK(ring.Full(picopaste::LogProducer::kMain));
  CHECK_FALSE(ring.Full(picopaste::LogProducer::kUploadWorker));
  CHECK_FALSE(ring.TryPush(picopaste::LogProducer::kMain,
                           picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "overflow")));
  CHECK(ring.Accepted() == picopaste::LogRing::Depth());
  CHECK(ring.Dropped() == 1U);
  CHECK(ring.Dropped(picopaste::LogProducer::kMain) == 1U);
}

TEST_CASE("LogRing keeps producers separate and drains round-robin", "[log]") {
  picopaste::LogRing ring;
  REQUIRE(ring.TryPush(picopaste::LogProducer::kMain, picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "main")));
  REQUIRE(ring.TryPush(picopaste::LogProducer::kSftpReader,
                       picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "sftp")));

  CHECK(ring.Size(picopaste::LogProducer::kMain) == 1U);
  CHECK(ring.Size(picopaste::LogProducer::kSftpReader) == 1U);
  CHECK(ring.Size() == 2U);

  picopaste::LogRecord out{};
  REQUIRE(ring.TryPop(out));
  CHECK(std::string(out.message) == "main");
  REQUIRE(ring.TryPop(out));
  CHECK(std::string(out.message) == "sftp");
  CHECK(ring.Empty());

  CHECK(ring.Accepted(picopaste::LogProducer::kMain) == 1U);
  CHECK(ring.Accepted(picopaste::LogProducer::kSftpReader) == 1U);
}

TEST_CASE("LogRing moves records across a producer and consumer thread", "[log]") {
  picopaste::LogRing ring;
  constexpr std::uint32_t kRecords = 2000;

  std::atomic<bool> done{false};
  std::uint64_t received = 0;

  std::thread producer([&ring, &done]() {
    for (std::uint32_t i = 0; i < kRecords; ++i) {
      // Wait for space, then push; TryPush counts only genuine overflow drops,
      // so the backpressure wait must not go through it.
      while (ring.Full(picopaste::LogProducer::kMain)) {
        std::this_thread::yield();
      }
      (void)ring.TryPush(picopaste::LogProducer::kMain,
                         picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "n=%u", i));
    }
    done.store(true, std::memory_order_release);
  });

  picopaste::LogRecord record{};
  while (!done.load(std::memory_order_acquire) || !ring.Empty()) {
    if (ring.TryPop(record)) {
      ++received;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();

  CHECK(received == kRecords);
  CHECK(ring.Dropped() == 0U);
}

TEST_CASE("LogSink rotates at the configured size and keeps bounded generations", "[log]") {
  TempDir dir("rotate");
  const std::string path = dir.file("app.log");
  constexpr std::uint32_t kMax = 300U;
  constexpr std::uint32_t kKeep = 2U;

  picopaste::LogSink sink;
  REQUIRE(sink.Open(path.c_str(), kMax, kKeep).has_value());
  CHECK(sink.IsOpen());
  CHECK(sink.MaxBytes() == kMax);

  for (std::uint32_t i = 0; i < 200U; ++i) {
    REQUIRE(sink.Write(picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "record number %u with some padding", i))
                .has_value());
  }
  const std::uint32_t rotations = sink.RotationCount();
  sink.Close();

  CHECK(rotations >= 2U);
  CHECK(std::filesystem::exists(path + ".1"));
  CHECK(std::filesystem::exists(path + ".2"));
  CHECK_FALSE(std::filesystem::exists(path + ".3"));

  // A file is rotated the moment it reaches the threshold; the largest a
  // generation can be is the threshold plus one line.
  constexpr std::uintmax_t kLineBytes = 320U;
  const std::uintmax_t bound = (static_cast<std::uintmax_t>(kMax) + kLineBytes) * (kKeep + 1U);
  const std::uintmax_t total = FileSize(path) + FileSize(path + ".1") + FileSize(path + ".2");
  CHECK(total <= bound);
  CHECK(total > 0U);
}

TEST_CASE("LogSink with keep_files=0 truncates in place", "[log]") {
  TempDir dir("truncate");
  const std::string path = dir.file("app.log");

  picopaste::LogSink sink;
  REQUIRE(sink.Open(path.c_str(), 200U, 0U).has_value());
  for (std::uint32_t i = 0; i < 40U; ++i) {
    REQUIRE(sink.Write(picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "line %u", i)).has_value());
  }
  sink.Close();

  CHECK(sink.RotationCount() > 0U);
  CHECK_FALSE(std::filesystem::exists(path + ".1"));
  CHECK(FileSize(path) < 200U + 320U);
}

TEST_CASE("LogSink reports writes before Open", "[log]") {
  picopaste::LogSink sink;
  CHECK_FALSE(sink.Write(picopaste::MakeLogRecord(picopaste::LogLevel::kInfo, "nope")).has_value());
}

#if defined(__linux__)

// Linux-only: memsample.hpp defines SampleMemory() under __linux__ alone (it
// reads /proc/self/status and /proc/self/fd). Windows declares the symbol but
// defines it in the win32 layer, so an unguarded case here fails to link.
// The assertions below are about /proc values, so there is nothing honest to
// check on another platform.
TEST_CASE("SampleMemory reports a live snapshot on Linux", "[log][memsample]") {
  const picopaste::MemorySnapshot snapshot = picopaste::SampleMemory();
  REQUIRE(snapshot.valid);
  CHECK(snapshot.working_set_bytes > 0U);
  CHECK(snapshot.peak_bytes >= snapshot.working_set_bytes);
  CHECK(snapshot.thread_count >= 1U);
  CHECK(snapshot.handle_count >= 1U);
}

#endif  // __linux__

// picopaste — remote settings.json hook management tests.
//
// These exercise a real SFTP round trip against this machine's own OpenSSH
// sftp-server (`ssh -s localhost sftp`), not a mock. Every case skips — rather
// than fails — when the subsystem is unavailable.
//
// Covered: fresh install into a missing file, merge that preserves user hooks,
// idempotent re-install, backup-content equality, restore, removal, and refusal
// on unparseable input leaving the file byte-for-byte untouched.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "test_support.hpp"

#include "../src/core/app/hook_install.hpp"
#include "../src/platform/posix/stream_posix.hpp"

using namespace picopaste;

namespace {

const char* const kSshArgv[] = {"ssh",
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

std::uint64_t NowMicros() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// Owns the channel and the connected settings session. Destruction order is
// explicit: the session is released before the stream is closed.
struct Env {
  sftp::ByteStream stream{};
  std::unique_ptr<RemoteSettings> rs;
  std::string dir;
  std::string settings;

  Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;
  ~Env() {
    rs.reset();
    if (stream.valid()) {
      stream.close(stream.ctx);
    }
  }
};

bool StartEnv(Env& env) {
  const auto spawned = posix::SpawnStream(kSshArgv);  if (!spawned.has_value()) {
    return false;
  }
  env.stream = spawned.value();

  auto conn = RemoteSettings::Connect(env.stream);
  if (!conn.has_value()) {
    return false;
  }
  env.rs = std::move(conn.value());

  const auto home = env.rs->Realpath(".");
  if (!home.has_value()) {
    return false;
  }
  env.dir = std::string(home.value().c_str()) + "/.cache/picopaste-test-hook-" +
            std::to_string(test::ProcessId()) + "-" + std::to_string(NowMicros());
  if (!env.rs->MkdirAll(env.dir.c_str())) {
    return false;
  }
  env.settings = env.dir + "/settings.json";
  return true;
}

HookEntry OurEntry() {
  HookEntry e;
  e.event = "PostToolUse";
  e.matcher = "*";
  e.command = "tee -a /tmp/picopaste-events.jsonl >/dev/null # picopaste-hook";
  return e;
}

std::string ReadAll(RemoteSettings& rs, const std::string& path, bool* missing = nullptr) {
  std::vector<std::uint8_t> bytes;
  const Status s = rs.ReadFile(path.c_str(), bytes, missing);
  REQUIRE(s);
  return std::string(bytes.begin(), bytes.end());
}

void WriteAll(RemoteSettings& rs, const std::string& path, const std::string& text) {
  const Status s = rs.WriteFile(path.c_str(), reinterpret_cast<const std::uint8_t*>(text.data()),
                                text.size());
  REQUIRE(s);
}

std::size_t CountOf(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0u;
  std::size_t pos = 0u;
  while ((pos = haystack.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

const std::string kUserSettings =
    "{\n"
    "  \"model\": \"sonnet\",\n"
    "  \"hooks\": {\n"
    "    \"PreToolUse\": [\n"
    "      {\n"
    "        \"matcher\": \"Bash\",\n"
    "        \"hooks\": [ { \"type\": \"command\", \"command\": \"echo user-pretool\" } ]\n"
    "      }\n"
    "    ],\n"
    "    \"PostToolUse\": [\n"
    "      {\n"
    "        \"matcher\": \"Write\",\n"
    "        \"hooks\": [ { \"type\": \"command\", \"command\": \"echo user-posttool\" } ]\n"
    "      }\n"
    "    ]\n"
    "  }\n"
    "}\n";

}  // namespace

TEST_CASE("hook install into a missing file creates it without a backup",
          "[hook][integration]") {
  Env env;
  if (!StartEnv(env)) {
    SKIP("local sftp subsystem unavailable");
  }

  bool missing = false;
  (void)ReadAll(*env.rs, env.settings, &missing);
  CHECK(missing);

  const auto out = env.rs->Install(env.settings.c_str(), OurEntry());
  REQUIRE(out.has_value());
  CHECK(out.value().changed);
  CHECK_FALSE(out.value().backup_created);

  const std::string merged = ReadAll(*env.rs, env.settings);
  CHECK(CountOf(merged, kHookMarker) == 1u);
  CHECK(merged.find("\"hooks\"") != std::string::npos);
  CHECK(merged.find("PostToolUse") != std::string::npos);
  CHECK(merged.find("tee -a /tmp/picopaste-events.jsonl") != std::string::npos);
}

TEST_CASE("hook install merges without clobbering and backs up the original",
          "[hook][integration]") {
  Env env;
  if (!StartEnv(env)) {
    SKIP("local sftp subsystem unavailable");
  }

  WriteAll(*env.rs, env.settings, kUserSettings);
  const std::string before = ReadAll(*env.rs, env.settings);
  REQUIRE(before == kUserSettings);

  const auto out = env.rs->Install(env.settings.c_str(), OurEntry());
  REQUIRE(out.has_value());
  CHECK(out.value().changed);
  REQUIRE(out.value().backup_created);

  const std::string merged = ReadAll(*env.rs, env.settings);
  CHECK(merged.find("echo user-pretool") != std::string::npos);
  CHECK(merged.find("echo user-posttool") != std::string::npos);
  CHECK(merged.find("\"model\"") != std::string::npos);
  CHECK(merged.find("sonnet") != std::string::npos);
  CHECK(CountOf(merged, kHookMarker) == 1u);

  bool backup_missing = true;
  const std::string backup = ReadAll(*env.rs, out.value().backup_path.c_str(), &backup_missing);
  CHECK_FALSE(backup_missing);
  CHECK(backup == before);  // backup is byte-for-byte the pre-write original
}

TEST_CASE("second hook install is an idempotent no-op", "[hook][integration]") {
  Env env;
  if (!StartEnv(env)) {
    SKIP("local sftp subsystem unavailable");
  }
  WriteAll(*env.rs, env.settings, kUserSettings);

  const auto first = env.rs->Install(env.settings.c_str(), OurEntry());
  REQUIRE(first.has_value());
  REQUIRE(first.value().changed);
  const std::string after_first = ReadAll(*env.rs, env.settings);

  const auto second = env.rs->Install(env.settings.c_str(), OurEntry());
  REQUIRE(second.has_value());
  CHECK_FALSE(second.value().changed);
  CHECK_FALSE(second.value().backup_created);
  CHECK(ReadAll(*env.rs, env.settings) == after_first);
  CHECK(CountOf(after_first, kHookMarker) == 1u);
}

TEST_CASE("restore puts the chosen backup back", "[hook][integration]") {
  Env env;
  if (!StartEnv(env)) {
    SKIP("local sftp subsystem unavailable");
  }
  WriteAll(*env.rs, env.settings, kUserSettings);

  const auto out = env.rs->Install(env.settings.c_str(), OurEntry());
  REQUIRE(out.has_value());
  REQUIRE(out.value().backup_created);

  const Status restored = env.rs->Restore(env.settings.c_str(), out.value().backup_path.c_str());
  REQUIRE(restored);
  CHECK(ReadAll(*env.rs, env.settings) == kUserSettings);
  CHECK(CountOf(ReadAll(*env.rs, env.settings), kHookMarker) == 0u);
}

TEST_CASE("hook removal strips our entry and keeps the user's", "[hook][integration]") {
  Env env;
  if (!StartEnv(env)) {
    SKIP("local sftp subsystem unavailable");
  }
  WriteAll(*env.rs, env.settings, kUserSettings);

  REQUIRE(env.rs->Install(env.settings.c_str(), OurEntry()).has_value());
  const auto removed = env.rs->Remove(env.settings.c_str(), kHookMarker);
  REQUIRE(removed.has_value());
  CHECK(removed.value());

  const std::string stripped = ReadAll(*env.rs, env.settings);
  CHECK(CountOf(stripped, kHookMarker) == 0u);
  CHECK(stripped.find("echo user-pretool") != std::string::npos);
  CHECK(stripped.find("echo user-posttool") != std::string::npos);

  const auto again = env.rs->Remove(env.settings.c_str(), kHookMarker);
  REQUIRE(again.has_value());
  CHECK_FALSE(again.value());
}

TEST_CASE("hook install refuses unparseable input and leaves the file untouched",
          "[hook][integration]") {
  Env env;
  if (!StartEnv(env)) {
    SKIP("local sftp subsystem unavailable");
  }

  const std::string cases[] = {
      "{ this is not json",
      "[1, 2, 3]",
      "{\"hooks\": 5}",
      "{\"hooks\": {\"PostToolUse\": 7}}",
      "{\"hooks\": {\"PostToolUse\": []}} trailing garbage",
  };

  for (const std::string& bad : cases) {
    WriteAll(*env.rs, env.settings, bad);
    const auto out = env.rs->Install(env.settings.c_str(), OurEntry());
    REQUIRE_FALSE(out.has_value());
    CHECK(out.get_error() == Error::kConfigParseFailed);
    CHECK(ReadAll(*env.rs, env.settings) == bad);  // untouched
  }
}

TEST_CASE("hook install rejects an entry without the marker", "[hook][integration]") {
  Env env;
  if (!StartEnv(env)) {
    SKIP("local sftp subsystem unavailable");
  }
  WriteAll(*env.rs, env.settings, kUserSettings);

  HookEntry bad;
  bad.event = "PostToolUse";
  bad.matcher = "*";
  bad.command = "echo no-marker-here";
  const auto out = env.rs->Install(env.settings.c_str(), bad);
  REQUIRE_FALSE(out.has_value());
  CHECK(out.get_error() == Error::kConfigParseFailed);
  CHECK(ReadAll(*env.rs, env.settings) == kUserSettings);
}

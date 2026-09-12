// picopaste — configuration load/save tests.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "picopaste/config.hpp"

namespace {

struct TempDir {
  std::filesystem::path path;

  explicit TempDir(const char* tag) {
    path = std::filesystem::temp_directory_path() / (std::string("picopaste_cfg_") + tag);
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

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

}  // namespace

TEST_CASE("DefaultConfig matches the documented defaults", "[config]") {
  const picopaste::Config cfg = picopaste::DefaultConfig();
  CHECK(std::string(cfg.remote_dir.c_str()) == "/tmp/picopaste");
  CHECK(std::string(cfg.hotkey.c_str()) == "alt+shift+v");
  CHECK(std::string(cfg.ssh_command.c_str()) == "ssh");
  CHECK(cfg.delay_ms == 150U);
  CHECK(cfg.log_max_bytes == 8U * 1024U * 1024U);
  CHECK(cfg.log_keep_files == 2U);
  CHECK(cfg.max_image_bytes == 20U * 1024U * 1024U);
  CHECK(cfg.job_memory_limit_mb == 32U);
  CHECK(cfg.restore_clipboard);
  CHECK(cfg.notify_enabled);
  CHECK(cfg.host.empty());
}

TEST_CASE("A missing file yields defaults and flags created_defaults", "[config]") {
  TempDir dir("missing");
  bool created = false;
  const auto result = picopaste::LoadConfig(dir.file("nope.ini").c_str(), &created);
  REQUIRE(result.has_value());
  CHECK(created);
  CHECK(result.value().delay_ms == 150U);
  CHECK(std::string(result.value().remote_dir.c_str()) == "/tmp/picopaste");
}

TEST_CASE("Save then Load round-trips every field", "[config]") {
  TempDir dir("roundtrip");
  const std::string path = dir.file("picopaste.ini");

  picopaste::Config cfg = picopaste::DefaultConfig();
  cfg.host = "my-remote";
  cfg.remote_dir = "~/pics/cc-clip";
  cfg.hotkey = "ctrl+alt+p";
  cfg.ssh_command = "tssh";
  cfg.delay_ms = 222U;
  cfg.restore_clipboard = false;
  cfg.max_image_bytes = 123456U;
  cfg.job_memory_limit_mb = 48U;
  cfg.log_max_bytes = 65536U;
  cfg.log_keep_files = 5U;
  cfg.log_level = "warn";
  cfg.notify_enabled = false;

  REQUIRE(picopaste::SaveConfig(path.c_str(), cfg).has_value());

  bool created = true;
  const auto loaded = picopaste::LoadConfig(path.c_str(), &created);
  REQUIRE(loaded.has_value());
  CHECK_FALSE(created);
  const picopaste::Config& got = loaded.value();
  CHECK(std::string(got.host.c_str()) == "my-remote");
  CHECK(std::string(got.remote_dir.c_str()) == "~/pics/cc-clip");
  CHECK(std::string(got.hotkey.c_str()) == "ctrl+alt+p");
  CHECK(std::string(got.ssh_command.c_str()) == "tssh");
  CHECK(got.delay_ms == 222U);
  CHECK_FALSE(got.restore_clipboard);
  CHECK(got.max_image_bytes == 123456U);
  CHECK(got.job_memory_limit_mb == 48U);
  CHECK(got.log_max_bytes == 65536U);
  CHECK(got.log_keep_files == 5U);
  CHECK(std::string(got.log_level.c_str()) == "warn");
  CHECK_FALSE(got.notify_enabled);
}

TEST_CASE("An over-long value is rejected, not truncated", "[config]") {
  TempDir dir("toolong");
  const std::string path = dir.file("picopaste.ini");

  SECTION("host exceeds 128 bytes") {
    WriteFile(path, "[picopaste]\nhost = " + std::string(200, 'h') + "\n");
    const auto result = picopaste::LoadConfig(path.c_str());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.get_error() == picopaste::Error::kConfigParseFailed);
  }

  SECTION("hotkey exceeds 64 bytes") {
    WriteFile(path, "[picopaste]\nhost = ok\nhotkey = " + std::string(100, 'k') + "\n");
    const auto result = picopaste::LoadConfig(path.c_str());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.get_error() == picopaste::Error::kConfigParseFailed);
  }

  SECTION("a value that exactly fits is accepted") {
    WriteFile(path, "[picopaste]\nhost = " + std::string(128, 'h') + "\n");
    const auto result = picopaste::LoadConfig(path.c_str());
    REQUIRE(result.has_value());
    CHECK(result.value().host.size() == 128U);
  }

  SECTION("remote_dir of 256 bytes is rejected") {
    WriteFile(path, "[picopaste]\nremote_dir = " + std::string(256, 'r') + "\n");
    const auto result = picopaste::LoadConfig(path.c_str());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.get_error() == picopaste::Error::kConfigParseFailed);
  }

  SECTION("remote_dir of 255 bytes is accepted") {
    WriteFile(path, "[picopaste]\nremote_dir = " + std::string(255, 'r') + "\n");
    const auto result = picopaste::LoadConfig(path.c_str());
    REQUIRE(result.has_value());
    CHECK(result.value().remote_dir.size() == 255U);
  }

  SECTION("ssh_command of 256 bytes is rejected") {
    WriteFile(path, "[picopaste]\nssh_command = " + std::string(256, 's') + "\n");
    const auto result = picopaste::LoadConfig(path.c_str());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.get_error() == picopaste::Error::kConfigParseFailed);
  }

  SECTION("ssh_command of 255 bytes is accepted") {
    WriteFile(path, "[picopaste]\nssh_command = " + std::string(255, 's') + "\n");
    const auto result = picopaste::LoadConfig(path.c_str());
    REQUIRE(result.has_value());
    CHECK(result.value().ssh_command.size() == 255U);
  }
}

TEST_CASE("SaveConfig is atomic and leaves no temp files on success", "[config]") {
  TempDir dir("atomic");
  const std::string path = dir.file("picopaste.ini");

  picopaste::Config cfg = picopaste::DefaultConfig();
  cfg.host = "original";
  REQUIRE(picopaste::SaveConfig(path.c_str(), cfg).has_value());

  for (const auto& entry : std::filesystem::directory_iterator(dir.path)) {
    CHECK(entry.path().extension() != ".tmp");
  }
  CHECK(ReadFile(path).find("original") != std::string::npos);
}

#if !defined(_WIN32)

// POSIX-only: the failure is forced by clearing write permission on the parent
// directory. Windows maps std::filesystem::permissions onto the read-only file
// attribute, which does not gate writes to a directory, so SaveConfig has no
// reason to fail and the assertions below cannot hold. The invariant itself
// (a failed save must not clobber the original) stays covered on Linux.
TEST_CASE("A failed atomic save leaves the original file intact", "[config]") {
  TempDir dir("readonly");
  const std::filesystem::path ro = dir.path / "ro";
  std::error_code ec;
  std::filesystem::create_directories(ro, ec);
  const std::string path = (ro / "picopaste.ini").string();

  picopaste::Config original = picopaste::DefaultConfig();
  original.host = "original";
  REQUIRE(picopaste::SaveConfig(path.c_str(), original).has_value());
  const std::string before = ReadFile(path);
  REQUIRE_FALSE(before.empty());

  std::filesystem::permissions(ro, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace);

  picopaste::Config changed = picopaste::DefaultConfig();
  changed.host = "changed";
  const auto result = picopaste::SaveConfig(path.c_str(), changed);
  CHECK_FALSE(result.has_value());
  CHECK(result.get_error() == picopaste::Error::kConfigWriteFailed);
  CHECK(ReadFile(path) == before);

  std::filesystem::permissions(ro, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
}

#endif  // !_WIN32

TEST_CASE("SaveConfig to a path in a nonexistent directory reports an error", "[config]") {
  TempDir dir("nodir");
  const std::string path = dir.file("does/not/exist/picopaste.ini");
  picopaste::Config cfg = picopaste::DefaultConfig();
  const auto result = picopaste::SaveConfig(path.c_str(), cfg);
  CHECK_FALSE(result.has_value());
}

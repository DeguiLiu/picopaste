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
 * @file config.cpp
 * @brief Runtime configuration load and save.
 *
 * Reads and writes the fixed-capacity `Config` through newosp's INI config
 * layer. The file is the only place configuration comes from; defaults live in
 * code so a fresh install needs no shipped template.
 *
 * Over-long values are rejected before assignment: `FixedString::assign` would
 * otherwise truncate silently, which the config contract forbids.
 */

#include "picopaste/config.hpp"

#include "osp/config.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace picopaste {
namespace {

using IniConfig = osp::Config<osp::IniBackend>;

constexpr const char* kSection = "picopaste";

// Locate `key`, tolerating a file written without a section header.
const char* Lookup(const IniConfig& cfg, const char* key) noexcept {
  if (cfg.HasKey(kSection, key)) {
    return cfg.GetString(kSection, key, "");
  }
  if (cfg.HasKey("", key)) {
    return cfg.GetString("", key, "");
  }
  return nullptr;
}

// Assign `key`'s value into `field`, rejecting anything that does not fit.
// A missing key leaves the field at its default.
template <std::uint32_t N>
Status AssignString(osp::FixedString<N>& field, const IniConfig& cfg, const char* key) noexcept {
  const char* raw = Lookup(cfg, key);
  if (raw == nullptr) {
    return Status::success();
  }
  if (std::strlen(raw) > static_cast<std::size_t>(N)) {
    return Status::error(Error::kConfigParseFailed);
  }
  field.assign(osp::TruncateToCapacity, raw);
  return Status::success();
}

std::uint32_t LookupU32(const IniConfig& cfg, const char* key, std::uint32_t fallback) noexcept {
  const std::int32_t value = cfg.GetInt(kSection, key, static_cast<std::int32_t>(fallback));
  if (0 > value) {
    return fallback;
  }
  return static_cast<std::uint32_t>(value);
}

bool LookupBool(const IniConfig& cfg, const char* key, bool fallback) noexcept {
  return cfg.GetBool(kSection, key, fallback);
}

// --- Raw value-length guard -------------------------------------------------
//
// newosp's ConfigStore stages every value in a char[256] and truncates it to
// 255 characters before returning it. For a field whose capacity is 255 that
// makes a 256-character value indistinguishable from a legitimate 255-character
// one. The contract says an over-long value is a reported error, so the raw
// file text is measured for the string fields before the parsed values are
// trusted. The scanner understands the INI shape this writer emits: `key =
// value` lines, `[...]` sections, and `;`/`#` comments.

constexpr std::uint32_t kRawFileBytes = 64U * 1024U;

bool KeyEquals(const char* text, std::size_t len, const char* key) noexcept {
  std::size_t i = 0;
  for (; (i < len) && ('\0' != key[i]); ++i) {
    char lhs = text[i];
    char rhs = key[i];
    if (('A' <= lhs) && ('Z' >= lhs)) {
      lhs = static_cast<char>(lhs + 32);
    }
    if (('A' <= rhs) && ('Z' >= rhs)) {
      rhs = static_cast<char>(rhs + 32);
    }
    if (lhs != rhs) {
      return false;
    }
  }
  return (i == len) && ('\0' == key[i]);
}

bool RawValueTooLong(const char* data, std::size_t size, const char* key, std::uint32_t capacity) noexcept {
  std::size_t line = 0;
  while (line < size) {
    std::size_t line_end = line;
    while ((line_end < size) && ('\n' != data[line_end])) {
      ++line_end;
    }
    std::size_t end = line_end;
    if ((end > line) && ('\r' == data[end - 1])) {
      --end;
    }

    std::size_t cursor = line;
    while ((cursor < end) && ((' ' == data[cursor]) || ('\t' == data[cursor]))) {
      ++cursor;
    }
    if ((cursor < end) && ('[' != data[cursor]) && (';' != data[cursor]) && ('#' != data[cursor])) {
      std::size_t eq = cursor;
      while ((eq < end) && ('=' != data[eq])) {
        ++eq;
      }
      if (eq < end) {
        std::size_t key_end = eq;
        while ((key_end > cursor) && ((' ' == data[key_end - 1]) || ('\t' == data[key_end - 1]))) {
          --key_end;
        }
        if (KeyEquals(data + cursor, key_end - cursor, key)) {
          std::size_t value = eq + 1;
          while ((value < end) && ((' ' == data[value]) || ('\t' == data[value]))) {
            ++value;
          }
          std::size_t value_end = end;
          for (std::size_t i = value; i < value_end; ++i) {
            if ((';' == data[i]) || ('#' == data[i])) {
              value_end = i;
              break;
            }
          }
          while ((value_end > value) && ((' ' == data[value_end - 1]) || ('\t' == data[value_end - 1]))) {
            --value_end;
          }
          if ((value_end - value) > static_cast<std::size_t>(capacity)) {
            return true;
          }
        }
      }
    }
    line = line_end + 1U;
  }
  return false;
}

bool FileHasOverlongValue(const char* path) noexcept {
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  // Known limit: only the first kRawFileBytes are scanned, so an over-long value
  // sitting past that offset is not caught and the library will truncate it
  // silently. Accepted deliberately: a real config is a few hundred bytes and
  // kRawFileBytes is 64 KB, while reading the whole file would mean allocating,
  // and this function is noexcept -- an allocation failure would terminate the
  // process, which is a worse outcome than the hole it would close.
  char buffer[kRawFileBytes];
  const std::size_t size = std::fread(buffer, 1, sizeof(buffer) - 1U, file);
  (void)std::fclose(file);
  buffer[size] = '\0';

  return RawValueTooLong(buffer, size, "host", kHostBytes) ||
         RawValueTooLong(buffer, size, "remote_dir", kRemoteDirBytes) ||
         RawValueTooLong(buffer, size, "hotkey", kHotkeyBytes) ||
         RawValueTooLong(buffer, size, "ssh_command", kSshCommandBytes) ||
         RawValueTooLong(buffer, size, "log_level", kLogLevelBytes);
}

bool WriteStr(std::FILE* file, const char* key, const char* value) noexcept {
  return 0 <= std::fprintf(file, "%s = %s\n", key, value);
}

bool WriteU32(std::FILE* file, const char* key, std::uint32_t value) noexcept {
  return 0 <= std::fprintf(file, "%s = %u\n", key, static_cast<unsigned>(value));
}

bool WriteBool(std::FILE* file, const char* key, bool value) noexcept {
  return 0 <= std::fprintf(file, "%s = %s\n", key, value ? "true" : "false");
}

// Flush kernel-side so a crash after rename cannot leave a torn file.
bool SyncFile(std::FILE* file) noexcept {
#if defined(_WIN32)
  return 0 == ::_commit(::_fileno(file));
#else
  return 0 == ::fsync(::fileno(file));
#endif
}

}  // namespace

/**
 * @brief Built-in configuration, used when no file exists and for every key a
 * file omits.
 */
Config DefaultConfig() noexcept {
  Config cfg;
  cfg.host.clear();
  cfg.remote_dir = "/tmp/picopaste";
  cfg.hotkey = "alt+shift+v";
  cfg.ssh_command = "ssh";
  cfg.log_level = "info";
  cfg.delay_ms = 150;
  cfg.upload_timeout_ms = 30000;
  cfg.log_max_bytes = 8u * 1024u * 1024u;
  cfg.log_keep_files = 2;
  cfg.max_image_bytes = 20u * 1024u * 1024u;
  cfg.job_memory_limit_mb = 32;
  cfg.restore_clipboard = true;
  cfg.notify_enabled = true;
  return cfg;
}

/**
 * @brief Load configuration from `path`.
 * @param path INI file to read; a null path is kConfigParseFailed.
 * @param created_defaults set to true when the file was missing, so the caller
 * can announce that defaults were created (may be null). A missing file is not
 * an error.
 * @return The parsed Config, or kConfigParseFailed for malformed or over-long
 * content.
 */
Result<Config> LoadConfig(const char* path, bool* created_defaults) noexcept {
  if (created_defaults != nullptr) {
    *created_defaults = false;
  }
  if (path == nullptr) {
    return Result<Config>::error(Error::kConfigParseFailed);
  }

  IniConfig ini;
  const auto loaded = ini.LoadFile(path);
  if (!loaded.has_value()) {
    const bool missing = (loaded.get_error() == osp::ConfigError::kFileNotFound);
    if (missing && (created_defaults != nullptr)) {
      *created_defaults = true;
    }
    return missing ? Result<Config>::success(DefaultConfig()) : Result<Config>::error(Error::kConfigParseFailed);
  }

  // Do NOT delete as redundant with the capacity constants: newosp's
  // ConfigStore stages every value into a char[256] and clamps to 255, so a
  // 256+ character value reaches the parsed struct looking like a valid 255.
  if (FileHasOverlongValue(path)) {
    return Result<Config>::error(Error::kConfigParseFailed);
  }

  Config cfg = DefaultConfig();
  Status strings = Status::success();
  strings = AssignString(cfg.host, ini, "host");
  if (strings.has_value()) {
    strings = AssignString(cfg.remote_dir, ini, "remote_dir");
  }
  if (strings.has_value()) {
    strings = AssignString(cfg.hotkey, ini, "hotkey");
  }
  if (strings.has_value()) {
    strings = AssignString(cfg.ssh_command, ini, "ssh_command");
  }
  if (strings.has_value()) {
    strings = AssignString(cfg.log_level, ini, "log_level");
  }
  if (!strings.has_value()) {
    return Result<Config>::error(strings.get_error());
  }

  cfg.delay_ms = LookupU32(ini, "delay_ms", cfg.delay_ms);
  cfg.upload_timeout_ms = LookupU32(ini, "upload_timeout_ms", cfg.upload_timeout_ms);
  cfg.max_image_bytes = LookupU32(ini, "max_image_bytes", cfg.max_image_bytes);
  cfg.job_memory_limit_mb = LookupU32(ini, "job_memory_limit_mb", cfg.job_memory_limit_mb);
  cfg.log_max_bytes = LookupU32(ini, "log_max_bytes", cfg.log_max_bytes);
  cfg.log_keep_files = LookupU32(ini, "log_keep_files", cfg.log_keep_files);
  cfg.restore_clipboard = LookupBool(ini, "restore_clipboard", cfg.restore_clipboard);
  cfg.notify_enabled = LookupBool(ini, "notify_enabled", cfg.notify_enabled);

  return Result<Config>::success(cfg);
}

/**
 * @brief Write `cfg` to `path`.
 *
 * Writes a temp file in the same directory, flushes it to disk, then renames,
 * so a crash mid-write cannot leave a torn config.
 * @return kConfigWriteFailed on any I/O failure; the temp file is removed.
 */
Status SaveConfig(const char* path, const Config& cfg) noexcept {
  if (path == nullptr) {
    return Status::error(Error::kConfigWriteFailed);
  }

  // Temp file in the same directory so rename is same-filesystem and atomic.
  char tmp[4096];
  const std::int32_t written = std::snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if ((0 > written) || (static_cast<std::size_t>(written) >= sizeof(tmp))) {
    return Status::error(Error::kConfigWriteFailed);
  }

  std::FILE* file = std::fopen(tmp, "wb");
  if (file == nullptr) {
    return Status::error(Error::kConfigWriteFailed);
  }

  bool ok = (0 <= std::fprintf(file, "[picopaste]\n"));
  ok = ok && WriteStr(file, "host", cfg.host.c_str());
  ok = ok && WriteStr(file, "remote_dir", cfg.remote_dir.c_str());
  ok = ok && WriteStr(file, "hotkey", cfg.hotkey.c_str());
  ok = ok && WriteU32(file, "delay_ms", cfg.delay_ms);
  ok = ok && WriteU32(file, "upload_timeout_ms", cfg.upload_timeout_ms);
  ok = ok && WriteBool(file, "restore_clipboard", cfg.restore_clipboard);
  ok = ok && WriteU32(file, "max_image_bytes", cfg.max_image_bytes);
  ok = ok && WriteU32(file, "job_memory_limit_mb", cfg.job_memory_limit_mb);
  ok = ok && WriteU32(file, "log_max_bytes", cfg.log_max_bytes);
  ok = ok && WriteU32(file, "log_keep_files", cfg.log_keep_files);
  ok = ok && WriteStr(file, "log_level", cfg.log_level.c_str());
  ok = ok && WriteBool(file, "notify_enabled", cfg.notify_enabled);
  ok = ok && WriteStr(file, "ssh_command", cfg.ssh_command.c_str());
  if (ok) {
    ok = (0 == std::fflush(file));
  }
  if (ok) {
    ok = SyncFile(file);
  }
  if (0 != std::fclose(file)) {
    ok = false;
  }

  if (ok) {
    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    ok = !rename_ec;
  }
  if (!ok) {
    std::error_code remove_ec;
    std::filesystem::remove(tmp, remove_ec);
    return Status::error(Error::kConfigWriteFailed);
  }
  return Status::success();
}

}  // namespace picopaste

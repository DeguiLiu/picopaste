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
 * @file config.hpp
 * @brief Runtime configuration: fixed-capacity strings parsed once at start-up.
 *
 * Fixed-capacity strings only: the config is read once at start-up and must not
 * introduce heap traffic or an unbounded allocation driven by a file's
 * contents. Over-long values are reported, never silently truncated.
 */
#pragma once

#include "osp/vocabulary.hpp"
#include "picopaste/error.hpp"

#include <cstdint>

namespace picopaste {

// The 255-wide fields are 255, not 256, on purpose: newosp's ConfigStore copies
// every value into a `char[256]` staging buffer before we see it, so a value
// longer than 255 characters would already have been silently shortened by the
// library and we could never reject it. Keeping those capacities one below the
// staging buffer makes "too long" detectable instead of silently accepted. The
// narrower fields below are bounded by what the value actually is, not by the
// staging buffer.
inline constexpr std::uint32_t kHostBytes = 128;
inline constexpr std::uint32_t kRemoteDirBytes = 255;
inline constexpr std::uint32_t kHotkeyBytes = 64;
inline constexpr std::uint32_t kSshCommandBytes = 255;
inline constexpr std::uint32_t kLogLevelBytes = 16;

struct Config {
  // SSH Host alias from ~/.ssh/config. The client never edits that file.
  osp::FixedString<kHostBytes> host{};

  // Remote directory for pasted images. Leading "~/" is resolved on the remote
  // via SFTP REALPATH, not by a shell.
  osp::FixedString<kRemoteDirBytes> remote_dir{};

  // Global hotkey, e.g. "alt+shift+v". Parsed locally; must not collide with
  // the simulated paste chord (ctrl+shift+v) or the system paste (ctrl+v).
  osp::FixedString<kHotkeyBytes> hotkey{};

  // Milliseconds between putting the path on the clipboard and sending the
  // keystroke. Covers the terminal's own paste handling.
  std::uint32_t delay_ms = 150;

  // How long one upload may stay in flight before it is treated as failed. A
  // silent peer leaves the worker blocked in a read with no EOF, so this is the
  // only way out; the caller checks it at a wake-up it already has. Zero
  // disables the deadline (the old unbounded behaviour).
  std::uint32_t upload_timeout_ms = 30000;

  // Put the image back on the clipboard after the paste.
  bool restore_clipboard = true;

  // Refuse images larger than this instead of holding them.
  std::uint32_t max_image_bytes = 20u * 1024u * 1024u;

  // Hard commit ceiling enforced by a Job Object on this process. Sized after
  // measuring the real baseline; see selftest.
  std::uint32_t job_memory_limit_mb = 32;

  // Bounded logging: rotate at log_max_bytes, keep log_keep_files generations.
  std::uint32_t log_max_bytes = 8u * 1024u * 1024u;
  std::uint32_t log_keep_files = 2;
  osp::FixedString<kLogLevelBytes> log_level{};

  // Notification bridge (M5). Off leaves the SFTP/event channels untouched.
  bool notify_enabled = true;

  // The ssh program to spawn. Overridable so a user can point at a wrapper
  // (for example a tssh-based one); defaults to the system OpenSSH client.
  osp::FixedString<kSshCommandBytes> ssh_command{};
};

/**
 * @brief Defaults for a fresh install.
 * @return A Config with every field at its documented default.
 *
 * remote_dir defaults to "/tmp/picopaste" so the server's own tmp cleanup
 * reclaims uploads; it stays fully config-overridable.
 */
Config DefaultConfig() noexcept;

/**
 * @brief Parse `path` into a Config.
 * @param path File to read.
 * @param created_defaults When non-null, set true if no file existed so the
 *        caller knows to persist the returned defaults.
 * @return The parsed config; kConfigParseFailed when the file is malformed.
 *
 * A missing file is not an error: defaults are returned.
 */
Result<Config> LoadConfig(const char* path, bool* created_defaults = nullptr) noexcept;

/**
 * @brief Write `cfg` to `path` atomically (temp file + replace).
 * @param path Destination file.
 * @param cfg Configuration to serialise.
 * @return kConfigWriteFailed when the temp file cannot be created, written,
 *         or moved into place.
 */
Status SaveConfig(const char* path, const Config& cfg) noexcept;

}  // namespace picopaste

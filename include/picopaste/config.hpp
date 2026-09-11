// picopaste — runtime configuration.
//
// Fixed-capacity strings only: the config is read once at start-up and must not
// introduce heap traffic or an unbounded allocation driven by a file's
// contents. Over-long values are reported, never silently truncated.
#pragma once

#include <cstdint>

#include "picopaste/error.hpp"
#include "osp/vocabulary.hpp"

namespace picopaste {

// Capacities are 255, not 256, on purpose. newosp's ConfigStore copies every
// value into a `char[256]` staging buffer before we see it, so a value longer
// than 255 characters would already be silently shortened by the library and
// we could never reject it. Keeping our capacity one below the staging buffer
// makes "too long" detectable instead of silently accepted.
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

// Defaults for a fresh install. remote_dir defaults to
// "~/.cache/picopaste/uploads" for continuity with the upstream tool.
Config DefaultConfig() noexcept;

// Parse `path`. A missing file is not an error: defaults are returned and
// `created_defaults` (when non-null) is set so the caller can persist them.
Result<Config> LoadConfig(const char* path, bool* created_defaults = nullptr) noexcept;

// Write `cfg` to `path` atomically (temp file + replace).
Status SaveConfig(const char* path, const Config& cfg) noexcept;

}  // namespace picopaste

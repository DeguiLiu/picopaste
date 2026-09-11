// picopaste — remote ~/.claude/settings.json hook management over SFTP.
//
// The product installs one Claude Code hook entry on the remote so the remote
// can emit events, and removes it again cleanly. Everything travels over an
// SFTP subsystem channel: no shell, no jq, no remote scripts.
//
// Safety contract, in priority order:
//   1. The existing settings file is parsed and structurally validated before
//      anything is written. If it cannot be understood, the operation fails
//      and the file is left byte-for-byte untouched.
//   2. Before the original is touched, its exact bytes are copied to a
//      timestamped sibling and read back to verify the copy.
//   3. Merging never clobbers: the user's own hooks and unrelated settings are
//      preserved, and a second install of the same entry is a no-op.
//
// The remote client is synchronous and single-threaded by contract; callers
// must serialize every call on one RemoteSettings instance.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "picopaste/error.hpp"
#include "picopaste/sftp/client.hpp"
#include "picopaste/sftp/stream.hpp"

namespace picopaste {

// Hard cap on the remote settings file this module will read or write. Larger
// files are refused untouched instead of being buffered without bound.
inline constexpr std::size_t kMaxSettingsBytes = 256u * 1024u;

// Substring that marks a hook entry as ours. It is embedded in the installed
// command so a second install is a detectable no-op and removal is
// unambiguous. `HookEntry::command` must contain it.
inline constexpr const char* kHookMarker = "picopaste-hook";

// One Claude Code hook group: an event, a tool matcher, and the command the
// remote runs for that event.
struct HookEntry {
  const char* event = nullptr;    // e.g. "PostToolUse"
  const char* matcher = nullptr;  // tool-name matcher; "" means every tool
  const char* command = nullptr;  // remote command; must contain kHookMarker
};

struct InstallOutcome {
  // false means the entry was already present: nothing was backed up or
  // written, so the call is idempotent.
  bool changed = false;

  bool backup_created = false;
  sftp::Path backup_path{};  // meaningful only when backup_created is true
};

// A live SFTP channel dedicated to settings file management. Construct it with
// Connect(); it owns the SFTP conversation but never closes the caller's
// ByteStream. Non-movable because it owns the non-movable sftp::Client.
class RemoteSettings final {
 public:
  RemoteSettings(const RemoteSettings&) = delete;
  RemoteSettings& operator=(const RemoteSettings&) = delete;
  RemoteSettings(RemoteSettings&&) = delete;
  RemoteSettings& operator=(RemoteSettings&&) = delete;
  ~RemoteSettings() noexcept;

  // `stream` must be a freshly connected SFTP channel: Connect performs the
  // SSH_FXP_INIT/VERSION handshake and must be the first thing sent on it.
  static Result<std::unique_ptr<RemoteSettings>> Connect(sftp::ByteStream stream) noexcept;

  // Canonicalize a remote path (passthrough to the SFTP client).
  Result<sftp::Path> Realpath(const char* path) noexcept;

  // Create every missing level of `path` (mkdir -p), tolerating existing ones.
  Status MkdirAll(const char* path) noexcept;

  // Ensure `entry` is present exactly once in `settings_path`. Reads, validates
  // and backs up the original, then writes the merged document.
  // Refuses with kConfigParseFailed (file untouched) when the existing content
  // is not a JSON object of the shape Claude Code expects.
  Result<InstallOutcome> Install(const char* settings_path, const HookEntry& entry) noexcept;

  // Remove every hook whose command contains `marker`. Returns true when the
  // file changed. Backs up before writing, exactly like Install.
  Result<bool> Remove(const char* settings_path, const char* marker) noexcept;

  // Put `backup_path` back over `settings_path`. The current file is backed up
  // first, so a restore is itself reversible.
  Status Restore(const char* settings_path, const char* backup_path) noexcept;

  // Read a remote file, bounded by kMaxSettingsBytes. A missing file is not an
  // error: `out` is left empty and success is returned. Set `missing` when the
  // caller needs to distinguish "absent" from "empty".
  Status ReadFile(const char* path, std::vector<std::uint8_t>& out, bool* missing = nullptr) noexcept;

  // Overwrite a remote file in place (OPEN with CREAT|TRUNC; RENAME is not
  // overwrite-capable on OpenSSH). Callers that need reversibility use
  // Install/Remove/Restore, which back up first.
  Status WriteFile(const char* path, const std::uint8_t* data, std::size_t len) noexcept;

 private:
  explicit RemoteSettings(sftp::ByteStream stream);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace picopaste

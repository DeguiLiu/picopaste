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
 * @file hook_install.cpp
 * @brief Remote settings.json hook management (implementation).
 *
 * All wire access goes through sftp::Client, the single owner of the SFTP
 * channel: Connect performs the SSH_FXP_INIT/VERSION handshake and read/write
 * of a remote file go through Client::ReadFile / Client::WriteFile. This module
 * holds no request-id counter and speaks no packet format of its own; it reads
 * a remote file into a caller-owned, bounded buffer, merges the JSON, and
 * writes the result back. Every buffer is fixed-capacity and bounded by
 * kMaxSettingsBytes.
 */
#include "hook_install.hpp"

#include <cstdio>
#include <cstring>

#include <chrono>
#include <string>

/* PicoJSON is vendored verbatim and used without PICOJSON_USE_INT64: enabling
   integer storage triggers an upstream assignment-in-condition warning that
   -Werror rejects. JSON numbers therefore round-trip as doubles, which is
   exact for every integer below 2^53 and for ordinary decimal settings.
   At -O2+ GCC also raises a false-positive -Wmaybe-uninitialized on PicoJSON's
   value union; the warning is located inside the header, so a location-scoped
   pragma suppresses it without touching the vendored source. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include "../../../third_party/picojson/picojson.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace picopaste {
namespace {

Status Err(Error e) noexcept {
  return Status::error(e);
}

std::uint64_t NowMillis() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

Status MakeBackupPath(const char* settings_path, std::uint64_t stamp, sftp::Path& out) noexcept {
  char buf[sftp::kMaxPathBytes];
  const std::int32_t n = std::snprintf(buf, sizeof(buf), "%s.picopaste-backup-%llu", settings_path,
                                       static_cast<unsigned long long>(stamp));
  if ((n < 0) || (static_cast<std::size_t>(n) >= sizeof(buf))) {
    return Err(Error::kConfigWriteFailed); /* would overflow the remote path cap */
  }
  out.assign(osp::TruncateToCapacity, buf);
  return Status::success();
}

bool IsBlank(const std::vector<std::uint8_t>& bytes) noexcept {
  for (const std::uint8_t c : bytes) {
    if ((c != 0x20u) && (c != 0x09u) && (c != 0x0Au) && (c != 0x0Du) && (c != 0x0Cu) && (c != 0x0Bu)) {
      return false;
    }
  }
  return true;
}

// Parses `bytes` as one JSON value and requires the whole input to be consumed,
// so trailing garbage is a parse failure rather than silently ignored.
bool ParseRoot(const std::vector<std::uint8_t>& bytes, picojson::value& root) {
  std::size_t off = 0u;
  if ((bytes.size() >= 3u) && (bytes[0] == 0xEFu) && (bytes[1] == 0xBBu) && (bytes[2] == 0xBFu)) {
    off = 3u; /* tolerate a UTF-8 BOM rather than failing on it */
  }
  const std::string text(reinterpret_cast<const char*>(bytes.data()) + off, bytes.size() - off);
  std::string err;
  /* The iterator is not advanced in place: the returned position is where
     parsing stopped, and only trailing whitespace may follow it. */
  const std::string::const_iterator stop = picojson::parse(root, text.begin(), text.end(), &err);
  if (!err.empty()) {
    return false;
  }
  for (std::string::const_iterator it = stop; it != text.end(); ++it) {
    const char c = *it;
    if ((c != ' ') && (c != '\t') && (c != '\n') && (c != '\r')) {
      return false;
    }
  }
  return root.is<picojson::object>();
}

// A restore source must parse as the JSON object settings are expected to be;
// anything else is refused before the current file is touched.
Status ValidateBackupObject(const std::vector<std::uint8_t>& wanted) {
  picojson::value check;
  if (!ParseRoot(wanted, check)) {
    return Err(Error::kConfigParseFailed);
  }
  return Status::success();
}

// PicoJSON escapes every forward slash as "\/" when serializing. That is valid
// JSON but needlessly rewrites the user's unrelated string values, so the
// escape is undone. The scan is escape-aware: it only removes a backslash that
// immediately precedes a slash, leaving "\n", "\\" and the like intact (a
// literal backslash-then-slash encodes as "\\\/" and is preserved correctly).
void UnescapeSlashes(std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0u; i < text.size(); ++i) {
    if ((text[i] == '\\') && ((i + 1u) < text.size()) && (text[i + 1u] == '/')) {
      out.push_back('/');
      ++i;
    } else {
      out.push_back(text[i]);
    }
  }
  text.swap(out);
}

bool ContainsMarker(const picojson::value& v, const char* marker) {
  return picojson::value(v).serialize().find(marker) != std::string::npos;
}

// Resolves the group array for `event`, creating the "hooks" object and the
// event array when they are absent. Returns nullptr when either member exists
// but has the wrong JSON type, so the caller refuses to rewrite a file it does
// not understand.
picojson::array* EventGroups(picojson::object& obj, const char* event) {
  auto hooks_it = obj.find("hooks");
  if (hooks_it == obj.end()) {
    obj["hooks"] = picojson::value(picojson::object());
    hooks_it = obj.find("hooks");
  }
  if (!hooks_it->second.is<picojson::object>()) {
    return nullptr;
  }
  auto& hooks = hooks_it->second.get<picojson::object>();

  auto event_it = hooks.find(event);
  if (event_it == hooks.end()) {
    hooks[event] = picojson::value(picojson::array());
    event_it = hooks.find(event);
  }
  if (!event_it->second.is<picojson::array>()) {
    return nullptr;
  }
  return &event_it->second.get<picojson::array>();
}

// Builds the merged document for an install. `changed` is false when our entry
// is already present, in which case `out` is unspecified and nothing was
// mutated. Structural surprises (hooks not an object, event not an array) are
// reported as failures so the caller never rewrites a file it does not
// understand.
Status BuildInstall(const std::vector<std::uint8_t>& original, const HookEntry& entry, std::string& out,
                    bool& changed) {
  changed = false;
  if ((entry.event == nullptr) || (entry.event[0] == '\0') || (entry.command == nullptr) ||
      (std::strstr(entry.command, kHookMarker) == nullptr)) {
    return Err(Error::kConfigParseFailed);
  }

  picojson::value root;
  if (IsBlank(original)) {
    root = picojson::value(picojson::object());
  } else if (!ParseRoot(original, root)) {
    return Err(Error::kConfigParseFailed);
  }

  picojson::array* groups = EventGroups(root.get<picojson::object>(), entry.event);
  if (groups == nullptr) {
    return Err(Error::kConfigParseFailed);
  }
  if (ContainsMarker(picojson::value(*groups), kHookMarker)) {
    return Status::success(); /* already installed: no-op, no backup, no write */
  }

  picojson::object hook;
  hook["type"] = picojson::value(std::string("command"));
  hook["command"] = picojson::value(std::string(entry.command));
  picojson::array inner;
  inner.push_back(picojson::value(hook));

  picojson::object group;
  group["matcher"] = picojson::value(std::string(entry.matcher != nullptr ? entry.matcher : ""));
  group["hooks"] = picojson::value(inner);
  groups->push_back(picojson::value(group));

  out = root.serialize(true);
  UnescapeSlashes(out);
  out.push_back('\n');
  changed = true;
  return Status::success();
}

// Builds the document with every hook whose command contains `marker` removed.
// Empty groups left by removal are dropped; event arrays and the hooks object
// themselves are kept so unrelated structure survives.
Status BuildRemove(const std::vector<std::uint8_t>& original, const char* marker, std::string& out, bool& changed) {
  changed = false;
  if ((marker == nullptr) || (marker[0] == '\0') || IsBlank(original)) {
    return Status::success();
  }

  picojson::value root;
  if (!ParseRoot(original, root)) {
    return Err(Error::kConfigParseFailed);
  }
  auto& obj = root.get<picojson::object>();
  auto hooks_it = obj.find("hooks");
  if (hooks_it == obj.end()) {
    return Status::success();
  }
  if (!hooks_it->second.is<picojson::object>()) {
    return Err(Error::kConfigParseFailed);
  }
  auto& hooks = hooks_it->second.get<picojson::object>();

  for (auto& kv : hooks) {
    if (!kv.second.is<picojson::array>()) {
      continue; /* an unrelated non-array event entry is left untouched */
    }
    picojson::array kept_groups;
    for (auto& group : kv.second.get<picojson::array>()) {
      if (group.is<picojson::object>()) {
        auto& g = group.get<picojson::object>();
        auto inner_it = g.find("hooks");
        if ((inner_it != g.end()) && inner_it->second.is<picojson::array>()) {
          picojson::array kept_hooks;
          for (auto& h : inner_it->second.get<picojson::array>()) {
            bool ours = false;
            if (h.is<picojson::object>()) {
              auto& ho = h.get<picojson::object>();
              auto cmd_it = ho.find("command");
              if ((cmd_it != ho.end()) && cmd_it->second.is<std::string>() &&
                  (cmd_it->second.get<std::string>().find(marker) != std::string::npos)) {
                ours = true;
              }
            }
            if (ours) {
              changed = true;
            } else {
              kept_hooks.push_back(h);
            }
          }
          inner_it->second = picojson::value(kept_hooks);
          if (kept_hooks.empty()) {
            continue; /* drop a group that held only our entries */
          }
        }
      }
      kept_groups.push_back(group);
    }
    kv.second = picojson::value(kept_groups);
  }

  if (changed) {
    out = root.serialize(true);
    UnescapeSlashes(out);
    out.push_back('\n');
  }
  return Status::success();
}

}  // namespace

struct RemoteSettings::Impl {
  explicit Impl(sftp::ByteStream stream) : client_(stream) {}

  // Reads a remote file into a caller-owned, bounded vector. A missing file is
  // success with `out` empty and `missing` set. An oversized file surfaces as
  // kBufferTooSmall — a size problem must never masquerade as a parse problem.
  Status ReadRemote(const char* path, std::vector<std::uint8_t>& out, std::size_t cap, bool& missing) noexcept {
    out.clear();
    missing = false;
    if (cap > static_cast<std::size_t>(UINT32_MAX)) {
      return Err(Error::kBufferTooSmall);
    }
    const std::uint32_t capacity = static_cast<std::uint32_t>(cap);
    try {
      out.resize(cap); /* caller-owned buffer: Client::ReadFile writes in place */
    } catch (...) {
      return Err(Error::kOpenFailed);
    }
    std::uint32_t got = 0u;
    const Status s = client_.ReadFile(path, out.data(), capacity, got, missing);
    if (!s) {
      out.clear();
      return s;
    }
    out.resize(got);
    return Status::success();
  }

  Status WriteRemote(const char* path, const std::uint8_t* data, std::size_t len) noexcept {
    if (len > static_cast<std::size_t>(UINT32_MAX)) {
      return Err(Error::kConfigWriteFailed);
    }
    /* OPEN(TRUNC) replaces in place: OpenSSH RENAME is not overwrite-capable,
       and the caller has already taken a verified backup where it matters. */
    return client_.WriteFile(path, data, static_cast<std::uint32_t>(len));
  }

  // Reads the file back and compares it byte-for-byte with `expected`. This is
  // how a backup is proven to exist and match before the original is touched.
  Status VerifyBytes(const char* path, const std::uint8_t* expected, std::size_t len) noexcept {
    std::vector<std::uint8_t> got;
    bool missing = false;
    const Status read = ReadRemote(path, got, kMaxSettingsBytes, missing);
    if (!read) {
      return Err(Error::kStatFailed);
    }
    if (missing || (got.size() != len)) {
      return Err(Error::kSizeMismatch);
    }
    if ((len > 0u) && (std::memcmp(got.data(), expected, len) != 0)) {
      return Err(Error::kSizeMismatch);
    }
    return Status::success();
  }

  // Copies `original` to a timestamped sibling and reads the copy back. Nothing
  // is trusted as a backup until this byte-for-byte comparison succeeds.
  Status BackupOriginal(const char* path, const std::vector<std::uint8_t>& original, sftp::Path& backup) noexcept {
    const Status named = MakeBackupPath(path, NowMillis(), backup);
    if (!named) {
      return Err(named.get_error());
    }
    const Status wrote = WriteRemote(backup.c_str(), original.data(), original.size());
    if (!wrote) {
      return Err(Error::kConfigWriteFailed);
    }
    const Status verified = VerifyBytes(backup.c_str(), original.data(), original.size());
    if (!verified) {
      return Err(Error::kSizeMismatch);
    }
    return Status::success();
  }

  // Writes `payload` over `path` and verifies it by reading back. WriteFile
  // opens with CREAT|TRUNC, so a failed write or a failed verify may already
  // have destroyed the original; when `have_backup` is set the in-memory
  // original (byte-identical to the verified backup) is written back. A
  // rollback that itself fails is reported as kWriteFailed, distinct from the
  // original write/verify error, so the caller can tell "wrong content" from
  // "wrong content and the file could not be put back".
  Status WriteVerified(const char* path, const std::vector<std::uint8_t>& original, bool have_backup,
                       const std::uint8_t* payload, std::size_t len) noexcept {
    Status result = WriteRemote(path, payload, len);
    if (result) {
      result = VerifyBytes(path, payload, len);
    }
    if (!result) {
      if (have_backup) {
        const Status rolled = WriteRemote(path, original.data(), original.size());
        if (!rolled || !VerifyBytes(path, original.data(), original.size())) {
          return Err(Error::kWriteFailed);
        }
      }
      return Err(result.get_error());
    }
    return Status::success();
  }

  // The sequence every mutating entry point shares: preserve the original
  // (when one is worth keeping), then write `payload` and prove it landed.
  // `backup_out` receives the backup path when one was made.
  Status BackupThenWrite(const char* path, const std::vector<std::uint8_t>& original, bool want_backup,
                         const std::uint8_t* payload, std::size_t len, sftp::Path* backup_out) noexcept {
    bool have_backup = false;
    sftp::Path backup;
    if (want_backup) {
      const Status made = BackupOriginal(path, original, backup);
      if (!made) {
        return made;
      }
      have_backup = true;
    }
    const Status result = WriteVerified(path, original, have_backup, payload, len);
    if (!result) {
      return result;
    }
    if ((backup_out != nullptr) && have_backup) {
      *backup_out = backup;
    }
    return Status::success();
  }

  sftp::Client client_;
};

RemoteSettings::RemoteSettings(sftp::ByteStream stream) : impl_(new Impl(stream)) {}

RemoteSettings::~RemoteSettings() noexcept = default;

Result<std::unique_ptr<RemoteSettings>> RemoteSettings::Connect(sftp::ByteStream stream) noexcept {
  if (!stream.valid()) {
    return Result<std::unique_ptr<RemoteSettings>>::error(Error::kChannelNotConnected);
  }
  try {
    auto obj = std::unique_ptr<RemoteSettings>(new RemoteSettings(stream));
    const Status init = obj->impl_->client_.Init();
    if (!init) {
      return Result<std::unique_ptr<RemoteSettings>>::error(init.get_error());
    }
    return Result<std::unique_ptr<RemoteSettings>>::success(std::move(obj));
  } catch (...) {
    /* Allocation failure under the job's commit ceiling: report, never throw. */
    return Result<std::unique_ptr<RemoteSettings>>::error(Error::kChannelNotConnected);
  }
}

Result<sftp::Path> RemoteSettings::Realpath(const char* path) noexcept {
  return impl_->client_.Realpath(path);
}

Status RemoteSettings::MkdirAll(const char* path) noexcept {
  return impl_->client_.MkdirAll(path);
}

Status RemoteSettings::ReadFile(const char* path, std::vector<std::uint8_t>& out, bool* missing) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return Err(Error::kOpenFailed);
  }
  bool was_missing = false;
  const Status s = impl_->ReadRemote(path, out, kMaxSettingsBytes, was_missing);
  if (missing != nullptr) {
    *missing = was_missing;
  }
  return s;
}

Status RemoteSettings::WriteFile(const char* path, const std::uint8_t* data, std::size_t len) noexcept {
  if ((path == nullptr) || (path[0] == '\0') || ((data == nullptr) && (len > 0u))) {
    return Err(Error::kWriteFailed);
  }
  if (len > kMaxSettingsBytes) {
    return Err(Error::kConfigWriteFailed);
  }
  try {
    return impl_->WriteRemote(path, data, len);
  } catch (...) {
    return Err(Error::kWriteFailed);
  }
}

Result<InstallOutcome> RemoteSettings::Install(const char* settings_path, const HookEntry& entry) noexcept {
  if ((settings_path == nullptr) || (settings_path[0] == '\0')) {
    return Result<InstallOutcome>::error(Error::kConfigWriteFailed);
  }
  try {
    std::vector<std::uint8_t> original;
    bool missing = false;
    Status status = impl_->ReadRemote(settings_path, original, kMaxSettingsBytes, missing);

    std::string merged;
    bool changed = false;
    if (status) {
      status = BuildInstall(original, entry, merged, changed); /* file untouched on failure */
    }
    if (!status) {
      return Result<InstallOutcome>::error(status.get_error());
    }

    InstallOutcome outcome;
    outcome.changed = changed;
    if (changed) {
      /* A blank or missing original has nothing worth preserving. */
      const bool want_backup = !missing && !IsBlank(original);
      sftp::Path backup;
      const Status wrote = impl_->BackupThenWrite(settings_path, original, want_backup,
                                                  reinterpret_cast<const std::uint8_t*>(merged.data()), merged.size(),
                                                  want_backup ? &backup : nullptr);
      if (!wrote) {
        return Result<InstallOutcome>::error(wrote.get_error());
      }
      outcome.backup_created = want_backup;
      outcome.backup_path = backup;
    }
    return Result<InstallOutcome>::success(outcome);
  } catch (...) {
    return Result<InstallOutcome>::error(Error::kConfigWriteFailed);
  }
}

Result<bool> RemoteSettings::Remove(const char* settings_path, const char* marker) noexcept {
  if ((settings_path == nullptr) || (settings_path[0] == '\0') || (marker == nullptr) || (marker[0] == '\0')) {
    return Result<bool>::error(Error::kConfigWriteFailed);
  }
  try {
    std::vector<std::uint8_t> original;
    bool missing = false;
    Status status = impl_->ReadRemote(settings_path, original, kMaxSettingsBytes, missing);

    std::string merged;
    bool changed = false;
    if (status && !missing) {
      status = BuildRemove(original, marker, merged, changed); /* file untouched on failure */
    }
    if (!status) {
      return Result<bool>::error(status.get_error());
    }

    bool removed = false;
    if (changed) {
      const bool want_backup = !IsBlank(original);
      const Status wrote =
          impl_->BackupThenWrite(settings_path, original, want_backup,
                                 reinterpret_cast<const std::uint8_t*>(merged.data()), merged.size(), nullptr);
      if (!wrote) {
        return Result<bool>::error(wrote.get_error());
      }
      removed = true;
    }
    return Result<bool>::success(removed);
  } catch (...) {
    return Result<bool>::error(Error::kConfigWriteFailed);
  }
}

Status RemoteSettings::Restore(const char* settings_path, const char* backup_path) noexcept {
  if ((settings_path == nullptr) || (settings_path[0] == '\0') || (backup_path == nullptr) ||
      (backup_path[0] == '\0')) {
    return Err(Error::kConfigWriteFailed);
  }
  try {
    std::vector<std::uint8_t> wanted;
    bool backup_missing = false;
    Status status = impl_->ReadRemote(backup_path, wanted, kMaxSettingsBytes, backup_missing);
    if (!status || backup_missing) {
      status = Err(Error::kOpenFailed);
    }
    if (status) {
      status = ValidateBackupObject(wanted);
    }

    std::vector<std::uint8_t> current;
    bool current_missing = false;
    if (status) {
      status = impl_->ReadRemote(settings_path, current, kMaxSettingsBytes, current_missing);
    }
    if (!status) {
      return status;
    }

    /* A blank or missing current file has nothing worth preserving. */
    const bool want_backup = !current_missing && !IsBlank(current);
    const Status wrote =
        impl_->BackupThenWrite(settings_path, current, want_backup, wanted.data(), wanted.size(), nullptr);
    if (!wrote) {
      return wrote;
    }
    return Status::success();
  } catch (...) {
    return Err(Error::kConfigWriteFailed);
  }
}

}  // namespace picopaste

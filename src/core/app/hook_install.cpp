// picopaste — remote settings.json hook management (see hook_install.hpp).
//
// All wire access goes through sftp::Client, the single owner of the SFTP
// channel: Connect performs the SSH_FXP_INIT/VERSION handshake and read/write
// of a remote file go through Client::ReadFile / Client::WriteFile. This module
// holds no request-id counter and speaks no packet format of its own; it reads
// a remote file into a caller-owned, bounded buffer, merges the JSON, and
// writes the result back. Every buffer is fixed-capacity and bounded by
// kMaxSettingsBytes.
#include "hook_install.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
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

Status Err(Error e) noexcept { return Status::error(e); }

std::uint64_t NowMillis() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

Status MakeBackupPath(const char* settings_path, std::uint64_t stamp, sftp::Path& out) noexcept {
  char buf[sftp::kMaxPathBytes];
  const int n = std::snprintf(buf, sizeof(buf), "%s.picopaste-backup-%llu", settings_path,
                              static_cast<unsigned long long>(stamp));
  if ((n < 0) || (static_cast<std::size_t>(n) >= sizeof(buf))) {
    return Err(Error::kConfigWriteFailed); /* would overflow the remote path cap */
  }
  out.assign(osp::TruncateToCapacity, buf);
  return Status::success();
}

bool IsBlank(const std::vector<std::uint8_t>& bytes) noexcept {
  for (const std::uint8_t c : bytes) {
    if ((c != 0x20u) && (c != 0x09u) && (c != 0x0Au) && (c != 0x0Du) && (c != 0x0Cu) &&
        (c != 0x0Bu)) {
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

// Builds the merged document for an install. `changed` is false when our entry
// is already present, in which case `out` is unspecified and nothing was
// mutated. Structural surprises (hooks not an object, event not an array) are
// reported as failures so the caller never rewrites a file it does not
// understand.
Status BuildInstall(const std::vector<std::uint8_t>& original, const HookEntry& entry,
                    std::string& out, bool& changed) {
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

  auto& obj = root.get<picojson::object>();
  auto hooks_it = obj.find("hooks");
  if (hooks_it == obj.end()) {
    obj["hooks"] = picojson::value(picojson::object());
    hooks_it = obj.find("hooks");
  }
  if (!hooks_it->second.is<picojson::object>()) {
    return Err(Error::kConfigParseFailed);
  }
  auto& hooks = hooks_it->second.get<picojson::object>();

  auto event_it = hooks.find(entry.event);
  if (event_it == hooks.end()) {
    hooks[entry.event] = picojson::value(picojson::array());
    event_it = hooks.find(entry.event);
  }
  if (!event_it->second.is<picojson::array>()) {
    return Err(Error::kConfigParseFailed);
  }
  auto& groups = event_it->second.get<picojson::array>();

  if (ContainsMarker(picojson::value(groups), kHookMarker)) {
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
  groups.push_back(picojson::value(group));

  out = root.serialize(true);
  UnescapeSlashes(out);
  out.push_back('\n');
  changed = true;
  return Status::success();
}

// Builds the document with every hook whose command contains `marker` removed.
// Empty groups left by removal are dropped; event arrays and the hooks object
// themselves are kept so unrelated structure survives.
Status BuildRemove(const std::vector<std::uint8_t>& original, const char* marker, std::string& out,
                   bool& changed) {
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
  Status ReadRemote(const char* path, std::vector<std::uint8_t>& out, std::size_t cap,
                    bool& missing) noexcept {
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

Status RemoteSettings::MkdirAll(const char* path) noexcept { return impl_->client_.MkdirAll(path); }

Status RemoteSettings::ReadFile(const char* path, std::vector<std::uint8_t>& out,
                                bool* missing) noexcept {
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

Status RemoteSettings::WriteFile(const char* path, const std::uint8_t* data,
                                 std::size_t len) noexcept {
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

Result<InstallOutcome> RemoteSettings::Install(const char* settings_path,
                                               const HookEntry& entry) noexcept {
  if ((settings_path == nullptr) || (settings_path[0] == '\0')) {
    return Result<InstallOutcome>::error(Error::kConfigWriteFailed);
  }
  try {
    std::vector<std::uint8_t> original;
    bool missing = false;
    const Status read = impl_->ReadRemote(settings_path, original, kMaxSettingsBytes, missing);
    if (!read) {
      return Result<InstallOutcome>::error(read.get_error());
    }

    std::string merged;
    bool changed = false;
    const Status built = BuildInstall(original, entry, merged, changed);
    if (!built) {
      return Result<InstallOutcome>::error(built.get_error()); /* file untouched */
    }

    InstallOutcome outcome;
    if (!changed) {
      return Result<InstallOutcome>::success(outcome); /* idempotent no-op */
    }

    if (!missing && !IsBlank(original)) {
      sftp::Path backup;
      const Status named = MakeBackupPath(settings_path, NowMillis(), backup);
      if (!named) {
        return Result<InstallOutcome>::error(named.get_error());
      }
      const Status wrote_backup = impl_->WriteRemote(backup.c_str(), original.data(), original.size());
      if (!wrote_backup) {
        return Result<InstallOutcome>::error(Error::kConfigWriteFailed);
      }
      const Status verified = impl_->VerifyBytes(backup.c_str(), original.data(), original.size());
      if (!verified) {
        return Result<InstallOutcome>::error(Error::kSizeMismatch);
      }
      outcome.backup_created = true;
      outcome.backup_path = backup;
    }

    const Status wrote =
        impl_->WriteRemote(settings_path, reinterpret_cast<const std::uint8_t*>(merged.data()),
                           merged.size());
    if (!wrote) {
      return Result<InstallOutcome>::error(Error::kConfigWriteFailed);
    }
    const Status verified =
        impl_->VerifyBytes(settings_path, reinterpret_cast<const std::uint8_t*>(merged.data()),
                           merged.size());
    if (!verified) {
      return Result<InstallOutcome>::error(Error::kSizeMismatch);
    }
    outcome.changed = true;
    return Result<InstallOutcome>::success(outcome);
  } catch (...) {
    return Result<InstallOutcome>::error(Error::kConfigWriteFailed);
  }
}

Result<bool> RemoteSettings::Remove(const char* settings_path, const char* marker) noexcept {
  if ((settings_path == nullptr) || (settings_path[0] == '\0') || (marker == nullptr) ||
      (marker[0] == '\0')) {
    return Result<bool>::error(Error::kConfigWriteFailed);
  }
  try {
    std::vector<std::uint8_t> original;
    bool missing = false;
    const Status read = impl_->ReadRemote(settings_path, original, kMaxSettingsBytes, missing);
    if (!read) {
      return Result<bool>::error(read.get_error());
    }
    if (missing) {
      return Result<bool>::success(false);
    }

    std::string merged;
    bool changed = false;
    const Status built = BuildRemove(original, marker, merged, changed);
    if (!built) {
      return Result<bool>::error(built.get_error()); /* file untouched */
    }
    if (!changed) {
      return Result<bool>::success(false);
    }

    if (!IsBlank(original)) {
      sftp::Path backup;
      const Status named = MakeBackupPath(settings_path, NowMillis(), backup);
      if (!named) {
        return Result<bool>::error(named.get_error());
      }
      const Status wrote_backup =
          impl_->WriteRemote(backup.c_str(), original.data(), original.size());
      if (!wrote_backup) {
        return Result<bool>::error(Error::kConfigWriteFailed);
      }
      const Status verified = impl_->VerifyBytes(backup.c_str(), original.data(), original.size());
      if (!verified) {
        return Result<bool>::error(Error::kSizeMismatch);
      }
    }

    const Status wrote =
        impl_->WriteRemote(settings_path, reinterpret_cast<const std::uint8_t*>(merged.data()),
                           merged.size());
    if (!wrote) {
      return Result<bool>::error(Error::kConfigWriteFailed);
    }
    const Status verified =
        impl_->VerifyBytes(settings_path, reinterpret_cast<const std::uint8_t*>(merged.data()),
                           merged.size());
    if (!verified) {
      return Result<bool>::error(Error::kSizeMismatch);
    }
    return Result<bool>::success(true);
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
    const Status read = impl_->ReadRemote(backup_path, wanted, kMaxSettingsBytes, backup_missing);
    if (!read || backup_missing) {
      return Err(Error::kOpenFailed);
    }
    /* Never restore something that is not the JSON object we expect. */
    {
      picojson::value check;
      if (!ParseRoot(wanted, check)) {
        return Err(Error::kConfigParseFailed);
      }
    }

    std::vector<std::uint8_t> current;
    bool current_missing = false;
    const Status read_current =
        impl_->ReadRemote(settings_path, current, kMaxSettingsBytes, current_missing);
    if (!read_current) {
      return Err(read_current.get_error());
    }
    if (!current_missing && !IsBlank(current)) {
      sftp::Path safety;
      const Status named = MakeBackupPath(settings_path, NowMillis(), safety);
      if (!named) {
        return Err(named.get_error());
      }
      const Status wrote = impl_->WriteRemote(safety.c_str(), current.data(), current.size());
      if (!wrote) {
        return Err(Error::kConfigWriteFailed);
      }
      const Status verified = impl_->VerifyBytes(safety.c_str(), current.data(), current.size());
      if (!verified) {
        return Err(Error::kSizeMismatch);
      }
    }

    const Status wrote = impl_->WriteRemote(settings_path, wanted.data(), wanted.size());
    if (!wrote) {
      return Err(Error::kConfigWriteFailed);
    }
    const Status verified = impl_->VerifyBytes(settings_path, wanted.data(), wanted.size());
    if (!verified) {
      return Err(Error::kSizeMismatch);
    }
    return Status::success();
  } catch (...) {
    return Err(Error::kConfigWriteFailed);
  }
}

}  // namespace picopaste

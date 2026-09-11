// picopaste — POSIX ByteStream over a child process pipe.
//
// The host test suite uses this to talk to the machine's own OpenSSH
// sftp-server via `ssh -s localhost sftp`. Not built on Windows; the win32
// workstream supplies the equivalent CreateProcessW-backed stream.
#pragma once

#include "picopaste/error.hpp"
#include "picopaste/sftp/stream.hpp"

namespace picopaste::posix {

// Spawns `argv[0]` with `argv` (must be nullptr-terminated), wiring the child's
// stdin/stdout to pipes, and returns a ByteStream over them. The child is
// reaped by ByteStream::close, which is idempotent.
//
// Returns kChannelSpawnFailed if the pipe or fork fails.
Result<sftp::ByteStream> SpawnStream(const char* const* argv) noexcept;

}  // namespace picopaste::posix

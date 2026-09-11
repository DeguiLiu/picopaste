// picopaste -- ByteStream over a CreateProcessW child with pipe redirection.
//
// This is the Windows counterpart of src/platform/posix/stream_posix.cpp: it
// spawns `ssh -o ClearAllForwardings=yes -s <host> sftp` with its stdin/stdout
// wired to anonymous pipes and exposes the pipes as a picopaste::sftp::ByteStream.
//
// The child is placed in the caller's containment Job Object (if one is given)
// so it cannot outlive us. read/write are blocking and exact-length, per the
// ByteStream contract.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// STARTUPINFOEX / PROC_THREAD_ATTRIBUTE_HANDLE_LIST need Vista or later.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

// See win32_util.hpp: osp/platform.hpp must precede <windows.h> so newosp does
// not mistake windows.h's RT_VERSION macro for the RT-Thread marker.
#include "osp/platform.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "picopaste/error.hpp"
#include "picopaste/sftp/stream.hpp"

namespace picopaste::win32 {

// Longest command line we will build/spawn. Generous enough for a host alias
// plus the ssh options; overflow is reported, never truncated silently.
inline constexpr std::uint32_t kMaxCommandLineChars = 1024;

struct ChildStreamOptions {
  // Full command line, e.g. L"ssh -o ClearAllForwardings=yes -s host sftp".
  // Interpreted with the same rules as CreateProcessW's lpCommandLine.
  const wchar_t* command_line = nullptr;
  // Working directory for the child; nullptr inherits ours.
  const wchar_t* working_dir = nullptr;
  // Containment job to assign the child to; nullptr to skip.
  HANDLE containment_job = nullptr;
};

// Owns the child process and the two pipe ends. Value type: the caller keeps it
// alive for the whole SFTP session and passes stream() to sftp::Client.
class ChildStream final {
 public:
  ChildStream() noexcept = default;
  ~ChildStream() noexcept { Close(); }
  ChildStream(const ChildStream&) = delete;
  ChildStream& operator=(const ChildStream&) = delete;

  // Spawn the child. Fails with kChannelSpawnFailed. On any failure no child or
  // handle is left behind.
  Status Spawn(const ChildStreamOptions& options) noexcept;

  // Terminate the child and close every handle. Idempotent; safe to call twice.
  void Close() noexcept;

  // A ByteStream backed by this object. Valid until Close() (or destruction).
  sftp::ByteStream stream() noexcept;

  HANDLE process_handle() const noexcept { return process_; }

  // Non-blocking: true while the child has not exited.
  bool running() const noexcept;

 private:
  static bool ReadThunk(void* ctx, std::uint8_t* dst, std::size_t len) noexcept;
  static bool WriteThunk(void* ctx, const std::uint8_t* data, std::size_t len) noexcept;
  static void CloseThunk(void* ctx) noexcept;

  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE write_end_ = nullptr;  // our end of the child's stdin
  HANDLE read_end_ = nullptr;   // our end of the child's stdout
  bool closed_ = true;
};

}  // namespace picopaste::win32

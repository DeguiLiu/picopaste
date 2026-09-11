// picopaste -- single-instance mutex and Job Object containment.
//
// Two jobs, deliberately:
//   1. a containment job with KILL_ON_JOB_CLOSE and NO memory limit, holding
//      the ssh.exe child (assigned explicitly at spawn) so this process's
//      death -- including a hard TerminateProcess -- cannot orphan the tunnel.
//      This process is deliberately NOT a member: closing the last handle to a
//      KILL_ON_JOB_CLOSE job terminates every associated process, so joining
//      that job ourselves would make Close() (and every early-return error
//      path) a self-kill;
//   2. a nested job carrying JOB_OBJECT_LIMIT_JOB_MEMORY, applied to this
//      process AND to its ssh.exe children: a child created by a process in a
//      job is itself in that job unless the job permits breakaway. That is
//      deliberate -- the runtime hard cap must cover the ssh child, which is
//      the largest allocator in the tree.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// See win32_util.hpp: osp/platform.hpp must precede <windows.h> so newosp does
// not mistake windows.h's RT_VERSION macro for the RT-Thread marker.
#include "osp/platform.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "picopaste/error.hpp"

namespace picopaste::win32 {

// Longest image path we record for the owning instance.
inline constexpr std::size_t kOwnerImageChars = 260;

// Session-scoped kernel object names. Local\ is deliberate: the Windows
// clipboard is per-session, so a second Windows session legitimately needs its
// own instance. The port is deliberately absent -- the requirement is a single
// process per session regardless of configuration.
inline constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\picopaste";
inline constexpr wchar_t kSingleInstanceOwnerName[] = L"Local\\picopaste.owner";

// Owns the named mutex, the owner-info section, and the two job objects for the
// process lifetime. Move-only-free: create one at start-up and keep it.
class SingleInstance final {
 public:
  SingleInstance() noexcept = default;
  ~SingleInstance() noexcept { Close(); }
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  // Create the session-wide mutex. On conflict returns kSingleInstanceExists
  // and, when the owner has published it, the owner's PID and image path in
  // the out params. A zero owner_pid with an empty image on that error means
  // the identity was explicitly unavailable, never that no owner exists.
  Status Acquire(std::uint32_t* owner_pid, wchar_t* owner_image,
                 std::size_t owner_image_chars) noexcept;

  // Create the containment job and the nested memory job. On any failure
  // returns kJobObjectFailed and tears down whichever job was created.
  Status SetupJobObjects(std::uint32_t memory_limit_mb) noexcept;

  // Containment job handle for the child spawner. Never closed by the caller.
  HANDLE containment_job() const noexcept { return containment_job_; }

  // Read back the enforced limits for the selftest report.
  Status QueryJobLimits(std::uint32_t* memory_limit_mb, bool* kill_on_close) noexcept;

  void Close() noexcept;

 private:
  HANDLE mutex_ = nullptr;
  HANDLE owner_section_ = nullptr;
  void* owner_view_ = nullptr;
  HANDLE containment_job_ = nullptr;
  HANDLE memory_job_ = nullptr;
};

}  // namespace picopaste::win32

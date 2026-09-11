// picopaste -- single-instance mutex and Job Object containment.
//
// Two jobs, deliberately:
//   1. a containment job with KILL_ON_JOB_CLOSE and NO memory limit, holding
//      this process and (by inheritance) its ssh.exe child, so a crash here
//      cannot orphan the tunnel;
//   2. a nested job carrying JOB_OBJECT_LIMIT_JOB_MEMORY, applied to this
//      process only. It also sets SILENT_BREAKAWAY_OK so the ssh.exe child is
//      not pulled into it -- otherwise the 32 MB ceiling would also cap
//      OpenSSH, which needs ~10 MB of its own.
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

// Owns the named mutex, the owner-info section, and the two job objects for the
// process lifetime. Move-only-free: create one at start-up and keep it.
class SingleInstance final {
 public:
  SingleInstance() noexcept = default;
  ~SingleInstance() noexcept { Close(); }
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  // Create Local\picopaste-<port>. On conflict returns kSingleInstanceExists
  // and, when available, the owner's PID and image path in the out params.
  Status Acquire(std::uint16_t port, std::uint32_t* owner_pid, wchar_t* owner_image,
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

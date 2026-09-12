// picopaste -- single-instance mutex and Job Object containment.
#include "single_instance.hpp"

#include <cstdlib>

#include "win32_util.hpp"

namespace picopaste::win32 {
namespace {

// Small page-file-backed section describing the current owner, so a second
// instance can report who holds the mutex. A mutex itself carries no PID. The
// owner fills pid/image and then publishes ready last, so a reader that sees
// ready == 0 must treat the identity as not yet available, never as a recorded
// owner with PID 0.
struct OwnerInfo {
  volatile std::uint32_t ready = 0;
  std::uint32_t pid = 0;
  wchar_t image[kOwnerImageChars] = {};
};

// The owner publishes within a few instructions of CreateMutexW returning, so a
// bounded wait closes the "not written yet" window without an unbounded spin.
// Past the bound the identity is reported as absent, which is explicit.
constexpr int kOwnerReadyAttempts = 20;
constexpr DWORD kOwnerReadySleepMs = 1;

void ReadOwner(std::uint32_t* owner_pid, wchar_t* owner_image,
               std::size_t owner_image_chars) noexcept {
  if (owner_pid == nullptr && owner_image == nullptr) {
    return;
  }
  for (int attempt = 0; attempt < kOwnerReadyAttempts; ++attempt) {
    UniqueHandle section(OpenFileMappingW(FILE_MAP_READ, FALSE, kSingleInstanceOwnerName));
    if (section.valid()) {
      const void* view = MapViewOfFile(section.get(), FILE_MAP_READ, 0, 0, sizeof(OwnerInfo));
      if (view != nullptr) {
        const OwnerInfo* info = static_cast<const OwnerInfo*>(view);
        const bool published = (info->ready != 0u);
        if (published) {
          // Acquire: pair with the writer's release barrier before the ready
          // store, so pid/image cannot be read from a half-written record.
          MemoryBarrier();
          if (owner_pid != nullptr) {
            *owner_pid = info->pid;
          }
          if (owner_image != nullptr && owner_image_chars > 0) {
            (void)CopyWide(info->image, owner_image, owner_image_chars);
          }
        }
        UnmapViewOfFile(view);
        if (published) {
          return;
        }
      }
    }
    Sleep(kOwnerReadySleepMs);
  }
}

void WriteOwner(HANDLE* section_out, void** view_out) noexcept {
  UniqueHandle section(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          sizeof(OwnerInfo), kSingleInstanceOwnerName));
  if (section.valid() == false) {
    return;
  }
  void* view = MapViewOfFile(section.get(), FILE_MAP_WRITE, 0, 0, sizeof(OwnerInfo));
  if (view == nullptr) {
    return;
  }
  OwnerInfo* info = static_cast<OwnerInfo*>(view);
  // Reset the marker before touching pid/image: a section reused from a
  // previous owner must not be readable as current until this write completes.
  info->ready = 0;
  MemoryBarrier();
  info->pid = GetCurrentProcessId();
  (void)GetModuleFileNameW(nullptr, info->image, static_cast<DWORD>(kOwnerImageChars));
  // Release: publish the completed record last.
  MemoryBarrier();
  info->ready = 1;
  *section_out = section.release();
  *view_out = view;
}

// Poll interval and ceiling for the restart wait. The ceiling exists so a
// predecessor that never exits cannot hang this process's start-up for ever.
constexpr DWORD kAwaitInstancePollMs = 50;

// Milliseconds to wait for the instance to be released when this process was
// relaunched by another one (kAwaitInstanceEnvName present and non-zero). Zero
// means an ordinary launch: report the conflict on the first attempt and exit.
unsigned long AwaitInstanceMs() noexcept {
  wchar_t value[16] = {};
  if (GetEnvironmentVariableW(kAwaitInstanceEnvName, value, 16) == 0) {
    return 0;
  }
  return (std::wcstoul(value, nullptr, 10) == 0) ? 0 : kAwaitInstanceMaxMs;
}

}  // namespace

Status SingleInstance::Acquire(std::uint32_t* owner_pid, wchar_t* owner_image,
                               std::size_t owner_image_chars) noexcept {
  if (owner_pid != nullptr) {
    *owner_pid = 0;
  }
  if (owner_image != nullptr && owner_image_chars > 0) {
    owner_image[0] = L'\0';
  }

  // Deliberately no initial owner and no WaitForSingleObject: the mutex object
  // simply exists while we do. The kernel releases it on process exit, which is
  // the exact lifetime we want, with no PID file to go stale.
  //
  // A restart relaunches this process before its predecessor has exited, so the
  // predecessor's mutex still exists for a moment. Waiting it out here is what
  // keeps "Restart" from silently degrading into "Quit"; the wait is bounded, and
  // with no such request in the environment it is zero, so the first attempt is
  // the only one and an ordinary second launch behaves exactly as before.
  const unsigned long await_ms = AwaitInstanceMs();
  unsigned long waited_ms = 0;
  for (;;) {
    mutex_ = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (mutex_ == nullptr) {
      return Status::error(Error::kJobObjectFailed);
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      break;
    }
    // Each failed attempt opens its own handle to the predecessor's mutex, and
    // an open handle keeps the object alive. Leaving it open would make the next
    // attempt -- and the predecessor's own exit -- irrelevant: we would be the
    // one holding the mutex. Close it before waiting.
    (void)CloseHandle(mutex_);
    mutex_ = nullptr;
    if (waited_ms >= await_ms) {
      ReadOwner(owner_pid, owner_image, owner_image_chars);
      return Status::error(Error::kSingleInstanceExists);
    }
    Sleep(kAwaitInstancePollMs);
    waited_ms += kAwaitInstancePollMs;
  }

  WriteOwner(&owner_section_, &owner_view_);
  return Status::success();
}

Status SingleInstance::SetupJobObjects(std::uint32_t memory_limit_mb) noexcept {
  if (containment_job_ != nullptr || memory_job_ != nullptr) {
    return Status::error(Error::kJobObjectFailed);
  }

  // 1. Containment: kill ssh.exe children when this process dies; no memory
  // limit here. This process is deliberately NOT assigned to this job. Closing
  // the last handle to a KILL_ON_JOB_CLOSE job terminates every process
  // associated with it ("Job Objects": "if the job has the
  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE flag specified, closing the last job
  // object handle terminates all associated processes"), so joining it would
  // turn Close(), the destructor, and every early-return error path below into
  // a self-kill that is impossible to observe. The child is assigned to this
  // job explicitly at spawn (stream_win32.cpp), and this handle is the only
  // one, so the kernel destroys the job -- killing the child -- when this
  // process terminates for any reason, including TerminateProcess.
  //
  // Rejected alternative: stay a member and never close the handle. That leaks
  // the handle by design, keeps a self-kill job attached to us for the whole
  // process lifetime, and leaves Close() unable to tear the child down
  // deterministically.
  containment_job_ = CreateJobObjectW(nullptr, nullptr);
  if (containment_job_ == nullptr) {
    return Status::error(Error::kJobObjectFailed);
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION containment_info{};
  containment_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (SetInformationJobObject(containment_job_, JobObjectExtendedLimitInformation, &containment_info,
                              sizeof(containment_info)) == 0) {
    Close();
    return Status::error(Error::kJobObjectFailed);
  }

  // 2. Nested memory ceiling, applied to this process AND to its ssh.exe
  // children. A child created by a process that is in a job is itself in that
  // job unless the job permits breakaway, so ssh.exe inherits this ceiling.
  // That is deliberate: the design requires the runtime hard cap to cover the
  // ssh child, which is the single largest allocator in the tree. Exceeding the
  // ceiling fails the allocation inside the child, which surfaces as an upload
  // failure -- visible, per the project's first requirement. Do NOT add
  // JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK here: it would let the memory-heavy
  // child out of the only cap that bounds it.
  //
  // BREAKAWAY_OK is the explicit form, not the silent one, and is what lets a
  // *restarted* instance escape this job and build its own: a process already in
  // a job cannot join an unrelated second one, so without it the relaunched child
  // fails in SetupJobObjects and the restart dies silently. The escape happens
  // only on an explicit CREATE_BREAKAWAY_FROM_JOB request, which ssh.exe does not
  // make -- it stays inside the ceiling.
  memory_job_ = CreateJobObjectW(nullptr, nullptr);
  if (memory_job_ == nullptr) {
    Close();
    return Status::error(Error::kJobObjectFailed);
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory_info{};
  memory_info.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
  memory_info.JobMemoryLimit = static_cast<SIZE_T>(memory_limit_mb) * 1024u * 1024u;
  if (SetInformationJobObject(memory_job_, JobObjectExtendedLimitInformation, &memory_info,
                              sizeof(memory_info)) == 0) {
    Close();
    return Status::error(Error::kJobObjectFailed);
  }
  if (AssignProcessToJobObject(memory_job_, GetCurrentProcess()) == 0) {
    // Fail loudly: a silently missing cap is worse than no start at all.
    Close();
    return Status::error(Error::kJobObjectFailed);
  }
  return Status::success();
}

Status SingleInstance::QueryJobLimits(std::uint32_t* memory_limit_mb,
                                      bool* kill_on_close) noexcept {
  if (memory_job_ == nullptr || containment_job_ == nullptr) {
    return Status::error(Error::kJobObjectFailed);
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory_info{};
  if (QueryInformationJobObject(memory_job_, JobObjectExtendedLimitInformation, &memory_info,
                                sizeof(memory_info), nullptr) == 0) {
    return Status::error(Error::kJobObjectFailed);
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION containment_info{};
  if (QueryInformationJobObject(containment_job_, JobObjectExtendedLimitInformation,
                                &containment_info, sizeof(containment_info), nullptr) == 0) {
    return Status::error(Error::kJobObjectFailed);
  }
  if (memory_limit_mb != nullptr) {
    *memory_limit_mb = static_cast<std::uint32_t>((memory_info.JobMemoryLimit + 1024u * 1024u - 1u) /
                                                  (1024u * 1024u));
  }
  if (kill_on_close != nullptr) {
    *kill_on_close =
        (containment_info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
  }
  return Status::success();
}

void SingleInstance::Close() noexcept {
  if (owner_view_ != nullptr) {
    UnmapViewOfFile(owner_view_);
    owner_view_ = nullptr;
  }
  if (owner_section_ != nullptr) {
    CloseHandle(owner_section_);
    owner_section_ = nullptr;
  }
  if (memory_job_ != nullptr) {
    CloseHandle(memory_job_);
    memory_job_ = nullptr;
  }
  if (containment_job_ != nullptr) {
    CloseHandle(containment_job_);
    containment_job_ = nullptr;
  }
  if (mutex_ != nullptr) {
    CloseHandle(mutex_);
    mutex_ = nullptr;
  }
}

}  // namespace picopaste::win32

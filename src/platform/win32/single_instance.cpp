// picopaste -- single-instance mutex and Job Object containment.
#include "single_instance.hpp"

#include <cwchar>

#include "win32_util.hpp"

namespace picopaste::win32 {
namespace {

// Small page-file-backed section describing the current owner, so a second
// instance can report who holds the mutex. A mutex itself carries no PID.
struct OwnerInfo {
  std::uint32_t pid = 0;
  std::uint32_t reserved = 0;
  wchar_t image[kOwnerImageChars] = {};
};

void MutexName(wchar_t* out, std::size_t chars, std::uint16_t port) noexcept {
  (void)swprintf(out, chars, L"Local\\picopaste-%u", static_cast<unsigned>(port));
}

void OwnerName(wchar_t* out, std::size_t chars, std::uint16_t port) noexcept {
  (void)swprintf(out, chars, L"Local\\picopaste-%u.owner", static_cast<unsigned>(port));
}

void ReadOwner(std::uint16_t port, std::uint32_t* owner_pid, wchar_t* owner_image,
               std::size_t owner_image_chars) noexcept {
  if (owner_pid == nullptr && owner_image == nullptr) {
    return;
  }
  wchar_t name[64] = {};
  OwnerName(name, 64, port);
  UniqueHandle section(OpenFileMappingW(FILE_MAP_READ, FALSE, name));
  if (section.valid() == false) {
    return;
  }
  const void* view = MapViewOfFile(section.get(), FILE_MAP_READ, 0, 0, sizeof(OwnerInfo));
  if (view == nullptr) {
    return;
  }
  const OwnerInfo* info = static_cast<const OwnerInfo*>(view);
  if (owner_pid != nullptr) {
    *owner_pid = info->pid;
  }
  if (owner_image != nullptr && owner_image_chars > 0) {
    (void)CopyWide(info->image, owner_image, owner_image_chars);
  }
  UnmapViewOfFile(view);
}

void WriteOwner(std::uint16_t port, HANDLE* section_out, void** view_out) noexcept {
  wchar_t name[64] = {};
  OwnerName(name, 64, port);
  UniqueHandle section(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          sizeof(OwnerInfo), name));
  if (section.valid() == false) {
    return;
  }
  void* view = MapViewOfFile(section.get(), FILE_MAP_WRITE, 0, 0, sizeof(OwnerInfo));
  if (view == nullptr) {
    return;
  }
  OwnerInfo* info = static_cast<OwnerInfo*>(view);
  info->pid = GetCurrentProcessId();
  info->reserved = 0;
  (void)GetModuleFileNameW(nullptr, info->image, static_cast<DWORD>(kOwnerImageChars));
  *section_out = section.release();
  *view_out = view;
}

}  // namespace

Status SingleInstance::Acquire(std::uint16_t port, std::uint32_t* owner_pid, wchar_t* owner_image,
                               std::size_t owner_image_chars) noexcept {
  if (owner_pid != nullptr) {
    *owner_pid = 0;
  }
  if (owner_image != nullptr && owner_image_chars > 0) {
    owner_image[0] = L'\0';
  }

  wchar_t mutex_name[64] = {};
  MutexName(mutex_name, 64, port);

  // Deliberately no initial owner and no WaitForSingleObject: the mutex object
  // simply exists while we do. The kernel releases it on process exit, which is
  // the exact lifetime we want, with no PID file to go stale.
  mutex_ = CreateMutexW(nullptr, FALSE, mutex_name);
  if (mutex_ == nullptr) {
    return Status::error(Error::kJobObjectFailed);
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    ReadOwner(port, owner_pid, owner_image, owner_image_chars);
    return Status::error(Error::kSingleInstanceExists);
  }

  WriteOwner(port, &owner_section_, &owner_view_);
  return Status::success();
}

Status SingleInstance::SetupJobObjects(std::uint32_t memory_limit_mb) noexcept {
  if (containment_job_ != nullptr || memory_job_ != nullptr) {
    return Status::error(Error::kJobObjectFailed);
  }

  // 1. Containment: kill children when we die. No memory limit here.
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
  if (AssignProcessToJobObject(containment_job_, GetCurrentProcess()) == 0) {
    Close();
    return Status::error(Error::kJobObjectFailed);
  }

  // 2. Nested memory ceiling, applied to this process only. SILENT_BREAKAWAY_OK
  // keeps the ssh.exe child out of this job so the ceiling does not also cap
  // OpenSSH; the child still inherits the containment job above.
  memory_job_ = CreateJobObjectW(nullptr, nullptr);
  if (memory_job_ == nullptr) {
    Close();
    return Status::error(Error::kJobObjectFailed);
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory_info{};
  memory_info.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
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

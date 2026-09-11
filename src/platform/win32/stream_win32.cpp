// picopaste -- ByteStream over a CreateProcessW child with pipe redirection.
#include "stream_win32.hpp"

#include <cstring>

#include "win32_util.hpp"

namespace picopaste::win32 {
namespace {

// STILL_ACTIVE, the exit code Windows reports for a running process.
constexpr DWORD kStillActive = 259u;

// A read or write that would transfer more than this in one kernel call is
// split. The SFTP layer hands us whole packets (at most one 64 KB chunk plus a
// header), so this is only a guard against an unexpectedly huge request.
constexpr std::size_t kMaxTransferPerCall = 1u << 20;

void CloseIfValid(HANDLE* handle) noexcept {
  if (*handle != nullptr) {
    CloseHandle(*handle);
    *handle = nullptr;
  }
}

}  // namespace

Status ChildStream::Spawn(const ChildStreamOptions& options) noexcept {
  Close();
  if (options.command_line == nullptr) {
    return Status::error(Error::kChannelSpawnFailed);
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  security.lpSecurityDescriptor = nullptr;

  HANDLE stdin_read = nullptr;
  HANDLE stdin_write = nullptr;
  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  if (CreatePipe(&stdin_read, &stdin_write, &security, 0) == 0) {
    return Status::error(Error::kChannelSpawnFailed);
  }
  if (CreatePipe(&stdout_read, &stdout_write, &security, 0) == 0) {
    CloseIfValid(&stdin_read);
    CloseIfValid(&stdin_write);
    return Status::error(Error::kChannelSpawnFailed);
  }

  // The ends we keep must not be inherited; only the child's ends may be, and
  // the handle list below pins exactly which ones cross the boundary.
  (void)SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
  (void)SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

  // ssh writes first-connection warnings and errors to stderr. Those must not
  // reach the SFTP stdout pipe or they would corrupt the framing, so stderr
  // goes to the NUL device.
  HANDLE nul_device = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &security, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOEXW startup{};
  BOOL have_attribute_list = FALSE;
  alignas(16) unsigned char attribute_buffer[256] = {};
  LPPROC_THREAD_ATTRIBUTE_LIST attribute_list =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer);

  SIZE_T required = 0;
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &required);
  if (required != 0 && required <= sizeof(attribute_buffer)) {
    if (InitializeProcThreadAttributeList(attribute_list, 1, 0, &required) != 0) {
      HANDLE inherit[3] = {stdin_read, stdout_write, nullptr};
      DWORD inherit_count = 2;
      if (nul_device != INVALID_HANDLE_VALUE) {
        inherit[2] = nul_device;
        inherit_count = 3;
      }
      if (UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    inherit, sizeof(HANDLE) * inherit_count, nullptr,
                                    nullptr) != 0) {
        have_attribute_list = TRUE;
      } else {
        DeleteProcThreadAttributeList(attribute_list);
      }
    }
  }

  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = stdin_read;
  startup.StartupInfo.hStdOutput = stdout_write;
  startup.StartupInfo.hStdError = (nul_device != INVALID_HANDLE_VALUE) ? nul_device : stdout_write;
  DWORD creation_flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
  if (have_attribute_list != FALSE) {
    startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    startup.lpAttributeList = attribute_list;
    creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
  } else {
    // Fallback: the two retained pipe ends are already non-inheritable, so the
    // only inheritable handles are the child's pipe ends and NUL.
    startup.StartupInfo.cb = sizeof(STARTUPINFO);
  }

  wchar_t command_line[kMaxCommandLineChars] = {};
  if (CopyWide(options.command_line, command_line, kMaxCommandLineChars) == false) {
    if (have_attribute_list != FALSE) {
      DeleteProcThreadAttributeList(attribute_list);
    }
    CloseIfValid(&stdin_read);
    CloseIfValid(&stdin_write);
    CloseIfValid(&stdout_read);
    CloseIfValid(&stdout_write);
    CloseIfValid(&nul_device);
    return Status::error(Error::kChannelSpawnFailed);
  }

  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(
      nullptr, command_line, nullptr, nullptr, TRUE, creation_flags, nullptr, options.working_dir,
      &startup.StartupInfo, &process_info);

  if (have_attribute_list != FALSE) {
    DeleteProcThreadAttributeList(attribute_list);
  }
  // Child-side ends are no longer needed in this process.
  CloseIfValid(&stdin_read);
  CloseIfValid(&stdout_write);
  CloseIfValid(&nul_device);

  if (created == 0) {
    CloseIfValid(&stdin_write);
    CloseIfValid(&stdout_read);
    return Status::error(Error::kChannelSpawnFailed);
  }

  CloseHandle(process_info.hThread);
  process_ = process_info.hProcess;
  write_end_ = stdin_write;
  read_end_ = stdout_read;
  closed_ = false;

  if (options.containment_job != nullptr) {
    // Best effort: the containment job also covers children implicitly, but an
    // explicit assignment makes the ownership independent of inheritance.
    (void)AssignProcessToJobObject(options.containment_job, process_);
  }
  return Status::success();
}

void ChildStream::Close() noexcept {
  if (closed_) {
    return;
  }
  closed_ = true;

  // Close our stdin end first so the child observes EOF and can exit cleanly.
  CloseIfValid(&write_end_);
  CloseIfValid(&read_end_);

  if (process_ != nullptr) {
    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_, &exit_code) != 0 && exit_code == kStillActive) {
      (void)TerminateProcess(process_, 1);
    }
    (void)WaitForSingleObject(process_, 2000);
    CloseHandle(process_);
    process_ = nullptr;
  }
  CloseIfValid(&thread_);
}

bool ChildStream::running() const noexcept {
  if (closed_ || process_ == nullptr) {
    return false;
  }
  DWORD exit_code = 0;
  if (GetExitCodeProcess(process_, &exit_code) == 0) {
    return false;
  }
  return exit_code == kStillActive;
}

bool ChildStream::WriteThunk(void* ctx, const std::uint8_t* data, std::size_t len) noexcept {
  ChildStream* self = static_cast<ChildStream*>(ctx);
  if (self == nullptr || self->closed_ || self->write_end_ == nullptr) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  std::size_t offset = 0;
  while (offset < len) {
    const std::size_t remaining = len - offset;
    const DWORD chunk =
        static_cast<DWORD>(remaining > kMaxTransferPerCall ? kMaxTransferPerCall : remaining);
    DWORD written = 0;
    if (WriteFile(self->write_end_, data + offset, chunk, &written, nullptr) == 0) {
      return false;
    }
    if (written == 0) {
      return false;
    }
    offset += written;
  }
  return true;
}

bool ChildStream::ReadThunk(void* ctx, std::uint8_t* dst, std::size_t len) noexcept {
  ChildStream* self = static_cast<ChildStream*>(ctx);
  if (self == nullptr || self->closed_ || self->read_end_ == nullptr) {
    return false;
  }
  if (len == 0) {
    return true;
  }
  std::size_t offset = 0;
  while (offset < len) {
    const std::size_t remaining = len - offset;
    const DWORD chunk =
        static_cast<DWORD>(remaining > kMaxTransferPerCall ? kMaxTransferPerCall : remaining);
    DWORD got = 0;
    if (ReadFile(self->read_end_, dst + offset, chunk, &got, nullptr) == 0) {
      return false;
    }
    if (got == 0) {
      return false;  // EOF before len bytes.
    }
    offset += got;
  }
  return true;
}

void ChildStream::CloseThunk(void* ctx) noexcept {
  ChildStream* self = static_cast<ChildStream*>(ctx);
  if (self != nullptr) {
    self->Close();
  }
}

sftp::ByteStream ChildStream::stream() noexcept {
  sftp::ByteStream out{};
  out.ctx = this;
  out.write = &ChildStream::WriteThunk;
  out.read = &ChildStream::ReadThunk;
  out.close = &ChildStream::CloseThunk;
  return out;
}

}  // namespace picopaste::win32

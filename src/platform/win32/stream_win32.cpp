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
 * @file stream_win32.cpp
 * @brief ByteStream over a CreateProcessW child with pipe redirection.
 */
#include "stream_win32.hpp"

#include "win32_util.hpp"

#include <cstring>

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

// Create the child's stdin/stdout pipe pairs. On failure no handle is left
// behind, so the caller only has to report the error.
bool CreateChildPipes(SECURITY_ATTRIBUTES* security, HANDLE* stdin_read, HANDLE* stdin_write, HANDLE* stdout_read,
                      HANDLE* stdout_write) noexcept {
  if (CreatePipe(stdin_read, stdin_write, security, 0) == 0) {
    return false;
  }
  if (CreatePipe(stdout_read, stdout_write, security, 0) == 0) {
    CloseIfValid(stdin_read);
    CloseIfValid(stdin_write);
    return false;
  }
  return true;
}

// Create the child's pipe pairs, open NUL for its stderr, and mark the two
// ends the parent keeps as non-inheritable. On failure every handle opened
// here is closed, so the caller only has to report the error. Non-inheritable
// parent ends are load-bearing: in the no-attribute-list fallback the child
// would otherwise inherit stdin_write / stdout_read, the parent would never
// observe EOF, and both pipes would stay open after Close().
bool CreateChildChannel(SECURITY_ATTRIBUTES* security, HANDLE* stdin_read, HANDLE* stdin_write, HANDLE* stdout_read,
                        HANDLE* stdout_write, HANDLE* nul_device) noexcept {
  if (CreateChildPipes(security, stdin_read, stdin_write, stdout_read, stdout_write) == false) {
    return false;
  }
  if ((SetHandleInformation(*stdin_write, HANDLE_FLAG_INHERIT, 0) == 0) ||
      (SetHandleInformation(*stdout_read, HANDLE_FLAG_INHERIT, 0) == 0)) {
    CloseIfValid(stdin_read);
    CloseIfValid(stdin_write);
    CloseIfValid(stdout_read);
    CloseIfValid(stdout_write);
    return false;
  }
  // ssh writes first-connection warnings and errors to stderr. Those must not
  // reach the SFTP stdout pipe or they would corrupt the framing, so stderr
  // goes to the NUL device. There is no second-best handle: pointing stderr at
  // the stdout pipe would put ssh's warnings inside a frame and desynchronise
  // the protocol, which is silent and looks like a remote fault. A NUL that
  // cannot be opened therefore fails the spawn instead.
  *nul_device =
      CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, security, OPEN_EXISTING, 0, nullptr);
  if (*nul_device == INVALID_HANDLE_VALUE) {
    CloseIfValid(stdin_read);
    CloseIfValid(stdin_write);
    CloseIfValid(stdout_read);
    CloseIfValid(stdout_write);
    return false;
  }
  return true;
}

// A null or over-long command line is refused here, giving Spawn a single
// failure point that runs before any handle is opened.
bool PrepareCommandLine(const ChildStreamOptions& options, wchar_t* out, std::size_t out_chars) noexcept {
  return (nullptr != options.command_line) && CopyWide(options.command_line, out, out_chars);
}

}  // namespace

Status ChildStream::Spawn(const ChildStreamOptions& options) noexcept {
  Close();
  wchar_t command_line[kMaxCommandLineChars] = {};
  if (PrepareCommandLine(options, command_line, kMaxCommandLineChars) == false) {
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
  HANDLE nul_device = INVALID_HANDLE_VALUE;
  if (CreateChildChannel(&security, &stdin_read, &stdin_write, &stdout_read, &stdout_write, &nul_device) == false) {
    return Status::error(Error::kChannelSpawnFailed);
  }

  STARTUPINFOEXW startup{};
  BOOL have_attribute_list = FALSE;
  alignas(16) unsigned char attribute_buffer[256] = {};
  LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer);

  SIZE_T required = 0;
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &required);
  if (required != 0 && required <= sizeof(attribute_buffer)) {
    if (InitializeProcThreadAttributeList(attribute_list, 1, 0, &required) != 0) {
      HANDLE inherit[3] = {stdin_read, stdout_write, nul_device};
      const DWORD inherit_count = 3;
      if (UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                    sizeof(HANDLE) * inherit_count, nullptr, nullptr) != 0) {
        have_attribute_list = TRUE;
      } else {
        DeleteProcThreadAttributeList(attribute_list);
      }
    }
  }

  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = stdin_read;
  startup.StartupInfo.hStdOutput = stdout_write;
  startup.StartupInfo.hStdError = nul_device;
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

  PROCESS_INFORMATION process_info{};
  const BOOL created = CreateProcessW(nullptr, command_line, nullptr, nullptr, TRUE, creation_flags, nullptr,
                                      options.working_dir, &startup.StartupInfo, &process_info);

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
    // Explicit assignment is the only mechanism: the parent is deliberately not
    // a member of the containment job, so there is no inheritance to fall back
    // on. The job's sole handle is the parent's, so the kernel kills this child
    // with it whenever the parent dies.
    //
    // A failed assignment leaves the child alive and uncontained, which is the
    // one thing this call exists to prevent: it would outlive us. Kill it and
    // report the spawn as failed rather than run a child we cannot account for.
    if (AssignProcessToJobObject(options.containment_job, process_) == 0) {
      Close();  // terminates the child, closes both pipes and the handle
      return Status::error(Error::kChannelSpawnFailed);
    }
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
    const DWORD chunk = static_cast<DWORD>(remaining > kMaxTransferPerCall ? kMaxTransferPerCall : remaining);
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
    const DWORD chunk = static_cast<DWORD>(remaining > kMaxTransferPerCall ? kMaxTransferPerCall : remaining);
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

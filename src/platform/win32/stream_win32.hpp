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
 * @file stream_win32.hpp
 * @brief ByteStream over a CreateProcessW child with pipe redirection.
 *
 * This is the Windows counterpart of src/platform/posix/stream_posix.cpp: it
 * spawns `ssh -o ClearAllForwardings=yes -s <host> sftp` with its stdin/stdout
 * wired to anonymous pipes and exposes the pipes as a picopaste::sftp::ByteStream.
 *
 * The child is placed in the caller's containment Job Object (if one is given)
 * so it cannot outlive us. read/write are blocking and exact-length, per the
 * ByteStream contract.
 */
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
#include "picopaste/error.hpp"
#include "picopaste/sftp/stream.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

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

  /**
   * @brief Spawn the child.
   * @return kChannelSpawnFailed on failure; no child or handle is left behind.
   */
  Status Spawn(const ChildStreamOptions& options) noexcept;

  /** @brief Terminate the child and close every handle. Idempotent. */
  void Close() noexcept;

  /** @brief A ByteStream backed by this object, valid until Close() (or destruction). */
  sftp::ByteStream stream() noexcept;

  HANDLE process_handle() const noexcept { return process_; }

  /** @brief Non-blocking: true while the child has not exited. */
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

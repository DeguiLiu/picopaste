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
 * @file stream_posix.cpp
 * @brief POSIX ByteStream over a child process pipe (implementation).
 */
#include "stream_posix.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace picopaste::posix {
namespace {

/* Fixed pool: spawning a channel is a one-time action, not a hot path, and the
   test suite never holds more than a couple of streams at once. Avoids heap. */
constexpr std::int32_t kMaxStreams = 4;

struct PipeCtx {
  std::int32_t in_fd = -1;  /* parent writes the child's stdin */
  std::int32_t out_fd = -1; /* parent reads the child's stdout */
  pid_t pid = -1;
  bool used = false;
};

PipeCtx g_slots[kMaxStreams];

PipeCtx* AcquireSlot() noexcept {
  for (std::int32_t i = 0; i < kMaxStreams; ++i) {
    if (!g_slots[i].used) {
      g_slots[i].used = true;
      g_slots[i].in_fd = -1;
      g_slots[i].out_fd = -1;
      g_slots[i].pid = -1;
      return &g_slots[i];
    }
  }
  return nullptr;
}

void CloseFd(std::int32_t& fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

bool PipeWrite(void* ctx, const std::uint8_t* data, std::size_t len) noexcept {
  PipeCtx* c = static_cast<PipeCtx*>(ctx);
  if ((c == nullptr) || !c->used || (c->in_fd < 0)) {
    return false;
  }
  std::size_t off = 0u;
  while (off < len) {
    const ssize_t n = ::write(c->in_fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool PipeRead(void* ctx, std::uint8_t* dst, std::size_t len) noexcept {
  PipeCtx* c = static_cast<PipeCtx*>(ctx);
  if ((c == nullptr) || !c->used || (c->out_fd < 0)) {
    return false;
  }
  std::size_t off = 0u;
  while (off < len) {
    const ssize_t n = ::read(c->out_fd, dst + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false; /* EOF before the requested length */
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

void PipeClose(void* ctx) noexcept {
  PipeCtx* c = static_cast<PipeCtx*>(ctx);
  if ((c == nullptr) || !c->used) {
    return;
  }
  /* Closing stdin lets ssh see EOF and exit cleanly. */
  CloseFd(c->in_fd);
  if (c->pid > 0) {
    std::int32_t status = 0;
    while ((::waitpid(c->pid, &status, 0) < 0) && (errno == EINTR)) {}
    c->pid = -1;
  }
  CloseFd(c->out_fd);
  c->used = false;
}

}  // namespace

Result<sftp::ByteStream> SpawnStream(const char* const* argv) noexcept {
  if ((argv == nullptr) || (argv[0] == nullptr)) {
    return Result<sftp::ByteStream>::error(Error::kChannelSpawnFailed);
  }

  std::int32_t in_pipe[2] = {-1, -1};
  std::int32_t out_pipe[2] = {-1, -1};
  if ((::pipe2(in_pipe, O_CLOEXEC) != 0) || (::pipe2(out_pipe, O_CLOEXEC) != 0)) {
    CloseFd(in_pipe[0]);
    CloseFd(in_pipe[1]);
    CloseFd(out_pipe[0]);
    CloseFd(out_pipe[1]);
    return Result<sftp::ByteStream>::error(Error::kChannelSpawnFailed);
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    CloseFd(in_pipe[0]);
    CloseFd(in_pipe[1]);
    CloseFd(out_pipe[0]);
    CloseFd(out_pipe[1]);
    return Result<sftp::ByteStream>::error(Error::kChannelSpawnFailed);
  }

  if (pid == 0) {
    /* Child: stdin <- in_pipe read end, stdout -> out_pipe write end. */
    (void)::dup2(in_pipe[0], STDIN_FILENO);
    (void)::dup2(out_pipe[1], STDOUT_FILENO);
    (void)::close(in_pipe[0]);
    (void)::close(in_pipe[1]);
    (void)::close(out_pipe[0]);
    (void)::close(out_pipe[1]);
    (void)::execvp(argv[0], const_cast<char* const*>(argv));
    _exit(127);
  }

  /* Parent: keep the write end of in_pipe and the read end of out_pipe. */
  CloseFd(in_pipe[0]);
  CloseFd(out_pipe[1]);

  PipeCtx* slot = AcquireSlot();
  if (slot == nullptr) {
    CloseFd(in_pipe[1]);
    CloseFd(out_pipe[0]);
    std::int32_t status = 0;
    while ((::waitpid(pid, &status, 0) < 0) && (errno == EINTR)) {}
    return Result<sftp::ByteStream>::error(Error::kChannelSpawnFailed);
  }
  slot->in_fd = in_pipe[1];
  slot->out_fd = out_pipe[0];
  slot->pid = pid;

  sftp::ByteStream stream{};
  stream.ctx = slot;
  stream.write = &PipeWrite;
  stream.read = &PipeRead;
  stream.close = &PipeClose;
  return Result<sftp::ByteStream>::success(stream);
}

}  // namespace picopaste::posix

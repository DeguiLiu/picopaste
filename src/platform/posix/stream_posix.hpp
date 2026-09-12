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
 * @file stream_posix.hpp
 * @brief POSIX ByteStream over a child process pipe.
 *
 * The host test suite uses this to talk to the machine's own OpenSSH
 * sftp-server via `ssh -s localhost sftp`. Not built on Windows; the win32
 * workstream supplies the equivalent CreateProcessW-backed stream.
 */
#pragma once

#include "picopaste/error.hpp"
#include "picopaste/sftp/stream.hpp"

namespace picopaste::posix {

/**
 * @brief Spawns `argv[0]` with `argv`, wiring the child's stdin/stdout to pipes
 * and returning a ByteStream over them.
 * @param argv nullptr-terminated argument vector.
 * @return A ByteStream over the pipes; the stream's close() reaps the child and
 * is idempotent. kChannelSpawnFailed if the pipe or fork fails.
 */
Result<sftp::ByteStream> SpawnStream(const char* const* argv) noexcept;

}  // namespace picopaste::posix

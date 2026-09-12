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
 * @file stream.hpp
 * @brief Byte-stream contract for the SFTP subsystem channel.
 *
 * A function-pointer table rather than an abstract base class: newosp's stated
 * dispatch preference ranks function-pointer+ctx above virtual, and this lets
 * the SFTP client live in a .cpp instead of being a template instantiated
 * everywhere. Each platform supplies its own ctx:
 *
 *   posix  : a pipe child (`ssh -s <host> sftp`)        — used by host tests
 *   win32  : CreateProcessW child with pipe redirection  — used by the client
 *
 * Both `read` and `write` are BLOCKING and must be all-or-nothing: `read`
 * fills exactly `len` bytes or reports failure, `write` consumes exactly `len`
 * bytes or reports failure. Partial transfer is not a thing callers handle.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace picopaste::sftp {

struct ByteStream {
  void* ctx = nullptr;

  // Write exactly `len` bytes. Returns false on any short write or error.
  bool (*write)(void* ctx, const std::uint8_t* data, std::size_t len) noexcept = nullptr;

  // Read exactly `len` bytes into `dst`. Returns false on EOF before `len`.
  bool (*read)(void* ctx, std::uint8_t* dst, std::size_t len) noexcept = nullptr;

  // Release the underlying child/pipe. Must be idempotent.
  void (*close)(void* ctx) noexcept = nullptr;

  bool valid() const noexcept { return ctx != nullptr && write != nullptr && read != nullptr && close != nullptr; }
};

}  // namespace picopaste::sftp

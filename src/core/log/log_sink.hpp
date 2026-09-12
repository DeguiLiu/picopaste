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
 * @file log_sink.hpp
 * @brief Bounded log file with size-based rotation.
 *
 * The previous tool had no rotation and grew its log to 25 MB. Rotation lives
 * in-process by requirement: there is no external logrotate, script, or shell.
 *
 * Rotation happens inside Write(), on the writer thread that owns this object.
 * A signal handler must never call into it: stdio and rename() are not
 * async-signal-safe. Writers are single-threaded by contract.
 */

#pragma once

#include "log_ring.hpp"

#include "picopaste/error.hpp"

#include <cstdint>
#include <cstdio>

namespace picopaste {

class LogSink {
 public:
  LogSink() noexcept = default;
  ~LogSink() noexcept;
  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;

  /**
   * @brief Open (or create) `path` for append.
   * @param path Log file to open.
   * @param max_bytes Rotation threshold.
   * @param keep_files Rotated generations to retain (path.1 .. path.N).
   * @return kLogIoFailed when the path is invalid or cannot be opened.
   */
  Status Open(const char* path, std::uint32_t max_bytes, std::uint32_t keep_files) noexcept;

  /**
   * @brief Flush and release the file. Idempotent.
   */
  void Close() noexcept;

  /**
   * @brief Append one record.
   * @param record Record to write.
   * @return kLogIoFailed when the file is closed or the write/flush fails.
   *
   * Flushes so the on-disk size tracks the accounting.
   */
  Status Write(const LogRecord& record) noexcept;

  /**
   * @brief Append up to `count` records.
   * @param records Array of records.
   * @param count Number of records available.
   * @return How many were written before the first failure.
   */
  std::uint32_t WriteBatch(const LogRecord* records, std::uint32_t count) noexcept;

  bool IsOpen() const noexcept { return file_ != nullptr; }
  std::uint32_t CurrentSize() const noexcept { return current_size_; }
  std::uint32_t RotationCount() const noexcept { return rotations_; }
  std::uint32_t MaxBytes() const noexcept { return max_bytes_; }
  std::uint32_t KeepFiles() const noexcept { return keep_files_; }

 private:
  // Close, shift generations, reopen empty. Called from Write() only.
  Status Rotate() noexcept;

  static constexpr std::uint32_t kPathBytes = 512;

  std::FILE* file_ = nullptr;
  char path_[kPathBytes] = {};
  std::uint32_t max_bytes_ = 0;
  std::uint32_t keep_files_ = 0;
  std::uint32_t current_size_ = 0;
  std::uint32_t rotations_ = 0;
};

}  // namespace picopaste

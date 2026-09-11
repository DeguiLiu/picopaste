// picopaste — bounded log file with size-based rotation.
//
// The previous tool had no rotation and grew its log to 25 MB. Rotation lives
// in-process by requirement: there is no external logrotate, script, or shell.
//
// Rotation happens inside Write(), on the writer thread that owns this object.
// A signal handler must never call into it: stdio and rename() are not
// async-signal-safe. Writers are single-threaded by contract.

#pragma once

#include <cstdint>
#include <cstdio>

#include "picopaste/error.hpp"
#include "log_ring.hpp"

namespace picopaste {

class LogSink {
 public:
  LogSink() noexcept = default;
  ~LogSink() noexcept;
  LogSink(const LogSink&) = delete;
  LogSink& operator=(const LogSink&) = delete;

  // Open (or create) `path` for append. `max_bytes` is the rotation threshold,
  // `keep_files` the number of rotated generations to retain (path.1 .. path.N).
  Status Open(const char* path, std::uint32_t max_bytes, std::uint32_t keep_files) noexcept;

  // Flush and release the file. Idempotent.
  void Close() noexcept;

  // Append one record. Flushes so the on-disk size tracks the accounting.
  Status Write(const LogRecord& record) noexcept;

  // Append up to `count` records. Returns how many were written.
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

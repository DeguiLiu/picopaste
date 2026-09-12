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
 * @file log_sink.cpp
 * @brief Bounded log file implementation.
 */

#include "log_sink.hpp"

#include <cstring>
#include <ctime>

#include <filesystem>
#include <system_error>

namespace picopaste {
namespace {

const char* LevelTag(LogLevel level) noexcept {
  // LogLevel is contiguous from kDebug; an out-of-range value can only come
  // from a corrupted record and gets the placeholder.
  static constexpr const char* kTags[] = {"DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
  const std::uint32_t index = static_cast<std::uint32_t>(level);
  return (index < (sizeof(kTags) / sizeof(kTags[0]))) ? kTags[index] : "?????";
}

// Format one line into `out`. Returns the number of bytes to write (excluding
// the terminator), or 0 on formatting failure.
std::uint32_t FormatLine(const LogRecord& record, char* out, std::size_t cap) noexcept {
  std::tm local{};
  const std::time_t seconds = static_cast<std::time_t>(record.wallclock_sec);
#if defined(_WIN32)
  const errno_t err = ::localtime_s(&local, &seconds);
  if (0 != err) {
    return 0;
  }
#else
  if (nullptr == ::localtime_r(&seconds, &local)) {
    return 0;
  }
#endif
  const std::int32_t written =
      std::snprintf(out, cap, "[%04d-%02d-%02d %02d:%02d:%02d.%03u] [%s] %s\n", local.tm_year + 1900, local.tm_mon + 1,
                    local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
                    static_cast<unsigned>(record.wallclock_ms), LevelTag(record.level), record.message);
  if (0 >= written) {
    return 0;
  }
  const std::size_t len = static_cast<std::size_t>(written);
  return (len < cap) ? static_cast<std::uint32_t>(len) : static_cast<std::uint32_t>(cap - 1U);
}

}  // namespace

LogSink::~LogSink() noexcept {
  Close();
}

Status LogSink::Open(const char* path, std::uint32_t max_bytes, std::uint32_t keep_files) noexcept {
  Close();
  if (path == nullptr) {
    return Status::error(Error::kLogIoFailed);
  }
  const std::size_t len = std::strlen(path);
  if ((0 == len) || (len >= kPathBytes)) {
    return Status::error(Error::kLogIoFailed);
  }
  std::memcpy(path_, path, len + 1U);

  // Clamp to 1: a zero threshold would rotate on every record, so an unset
  // value means "smallest usable bound" rather than "disable rotation".
  max_bytes_ = (0 == max_bytes) ? 1U : max_bytes;
  keep_files_ = keep_files;
  rotations_ = 0;

  // Append so a restart continues the same file instead of erasing history.
  file_ = std::fopen(path_, "ab");
  if (file_ == nullptr) {
    return Status::error(Error::kLogIoFailed);
  }

  std::error_code size_ec;
  const auto existing = std::filesystem::file_size(path_, size_ec);
  current_size_ = size_ec ? 0U : static_cast<std::uint32_t>(existing);

  if (current_size_ >= max_bytes_) {
    return Rotate();
  }
  return Status::success();
}

void LogSink::Close() noexcept {
  if (file_ != nullptr) {
    (void)std::fclose(file_);
    file_ = nullptr;
  }
}

Status LogSink::Write(const LogRecord& record) noexcept {
  if (file_ == nullptr) {
    return Status::error(Error::kLogIoFailed);
  }

  // A formatted line is timestamp (23) + tag (5) + message (<= kLogMessageBytes)
  // plus separators; 320 leaves headroom without a second allocation.
  char line[320];
  const std::uint32_t len = FormatLine(record, line, sizeof(line));
  if (0 == len) {
    return Status::error(Error::kLogIoFailed);
  }
  if (len != std::fwrite(line, 1, len, file_)) {
    return Status::error(Error::kLogIoFailed);
  }
  // Flush so current_size_ is the real on-disk size, not an incoherent buffer.
  if (0 != std::fflush(file_)) {
    return Status::error(Error::kLogIoFailed);
  }
  current_size_ += len;

  if (current_size_ >= max_bytes_) {
    return Rotate();
  }
  return Status::success();
}

std::uint32_t LogSink::WriteBatch(const LogRecord* records, std::uint32_t count) noexcept {
  std::uint32_t written = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!Write(records[i]).has_value()) {
      break;
    }
    ++written;
  }
  return written;
}

Status LogSink::Rotate() noexcept {
  if (file_ != nullptr) {
    (void)std::fclose(file_);
    file_ = nullptr;
  }

  if (keep_files_ > 0U) {
    std::error_code ec;

    // Drop the oldest generation, then shift each one up by one.
    char oldest[kPathBytes + 16];
    (void)std::snprintf(oldest, sizeof(oldest), "%s.%u", path_, static_cast<unsigned>(keep_files_));
    std::filesystem::remove(oldest, ec);

    for (std::uint32_t i = keep_files_; i > 1U; --i) {
      char from[kPathBytes + 16];
      char to[kPathBytes + 16];
      (void)std::snprintf(from, sizeof(from), "%s.%u", path_, static_cast<unsigned>(i - 1U));
      (void)std::snprintf(to, sizeof(to), "%s.%u", path_, static_cast<unsigned>(i));
      std::error_code shift_ec;
      std::filesystem::rename(from, to, shift_ec);
    }

    char first[kPathBytes + 16];
    (void)std::snprintf(first, sizeof(first), "%s.1", path_);
    std::error_code move_ec;
    std::filesystem::rename(path_, first, move_ec);
  }

  file_ = std::fopen(path_, "wb");
  if (file_ == nullptr) {
    return Status::error(Error::kLogIoFailed);
  }
  current_size_ = 0;
  ++rotations_;
  return Status::success();
}

}  // namespace picopaste

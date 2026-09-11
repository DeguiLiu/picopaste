// picopaste — log record timestamping and formatting.
//
// Kept out of the header so the clock dependency is a single translation unit
// and the hot ring operations stay inline.

#include "log_ring.hpp"

#include <cstdarg>
#include <cstdio>

#include <chrono>

namespace picopaste {

void StampNow(LogRecord& record) noexcept {
  record.monotonic_us = osp::SteadyNowUs();

  const auto now = std::chrono::system_clock::now();
  const auto whole_seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto fraction_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - whole_seconds).count();

  record.wallclock_sec = static_cast<std::uint32_t>(std::chrono::system_clock::to_time_t(whole_seconds));
  record.wallclock_ms = static_cast<std::uint16_t>(fraction_ms);
}

LogRecord MakeLogRecord(LogLevel level, const char* fmt, ...) noexcept {
  LogRecord record;
  StampNow(record);
  record.level = level;

  va_list args;
  va_start(args, fmt);
  (void)std::vsnprintf(record.message, sizeof(record.message), fmt, args);
  va_end(args);
  return record;
}

}  // namespace picopaste

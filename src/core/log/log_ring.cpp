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
 * @file log_ring.cpp
 * @brief Log record timestamping and formatting.
 *
 * Kept out of the header so the clock dependency is a single translation unit
 * and the hot ring operations stay inline.
 */

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

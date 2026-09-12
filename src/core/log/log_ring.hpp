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
 * @file log_ring.hpp
 * @brief Fixed-capacity lock-free log queues, one per producer thread.
 *
 * The log writer is the single consumer. Four threads produce records, and an
 * SPSC ring tolerates exactly one producer, so each producer owns its own ring
 * (the same architecture newosp's async_log uses). The writer drains all rings
 * round-robin. No heap and no mutex on any path.
 *
 * Producers address their ring by a compile-time slot index (LogProducer), not
 * by a runtime thread-id lookup: that is allocation-free and makes the origin
 * of every record explicit at the call site.
 */

#pragma once

#include "osp/platform.hpp"
#include "osp/spsc_ringbuffer.hpp"

#include <cstdint>

#include <array>
#include <atomic>
#include <type_traits>

namespace picopaste {

enum class LogLevel : std::uint8_t { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3, kFatal = 4 };

// One slot per thread that logs, with the record classes each owns:
//   kMain        — startup/config, hotkey registration, single instance, tray.
//   kSftpReader  — channel establishment, pipe EOF/loss, transport/protocol errors.
//   kUploadWorker— clipboard capture, upload steps, size mismatch, paste result.
//   kLogWriter   — the logging subsystem's own rotation/IO failures and drops.
enum class LogProducer : std::uint8_t {
  kMain = 0,
  kSftpReader = 1,
  kUploadWorker = 2,
  kLogWriter = 3,
};

inline constexpr std::uint32_t kLogProducerCount = 4;

inline constexpr std::uint32_t kLogMessageBytes = 160;

// Per-producer depth, power of two as SpscRingbuffer requires. Four rings of
// 32 records (~176 B each) is ~22 KB, inside the design's 64 KB log budget.
inline constexpr std::uint32_t kLogRingDepth = 32;

struct LogRecord {
  std::uint64_t monotonic_us = 0;  // Ordering only; not wall-clock.
  std::uint32_t wallclock_sec = 0;
  std::uint16_t wallclock_ms = 0;
  LogLevel level = LogLevel::kInfo;
  std::uint8_t reserved = 0;
  char message[kLogMessageBytes] = {};
};

static_assert(std::is_trivially_copyable<LogRecord>::value, "LogRecord must stay trivially copyable");

/**
 * @brief Fill a record's timestamp fields from the platform clock.
 * @param record Record whose monotonic_us / wallclock_sec / wallclock_ms are set.
 *
 * Wall-clock resolution is milliseconds.
 */
void StampNow(LogRecord& record) noexcept;

/**
 * @brief Format `fmt` into a record and stamp it.
 * @param level Severity stored with the record.
 * @param fmt printf-style format string.
 * @return The formatted record. Never allocates; the message is truncated to
 *         kLogMessageBytes, which is a bounded, self-inflicted loss.
 */
LogRecord MakeLogRecord(LogLevel level, const char* fmt, ...) noexcept OSP_PRINTF_FMT(2, 3);

class LogRing {
 public:
  LogRing() noexcept {
    for (std::uint32_t i = 0; i < kLogProducerCount; ++i) {
      accepted_[i].store(0, std::memory_order_relaxed);
      dropped_[i].store(0, std::memory_order_relaxed);
    }
  }
  LogRing(const LogRing&) = delete;
  LogRing& operator=(const LogRing&) = delete;

  /**
   * @brief Producer side: push one record onto this producer's ring.
   * @param producer The pushing thread's compile-time slot.
   * @param record Record to enqueue.
   * @return False when the ring is full; the record is dropped and counted.
   *
   * Each slot must be pushed by exactly one thread.
   */
  bool TryPush(LogProducer producer, const LogRecord& record) noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(producer);
    if (slot >= kLogProducerCount) {
      return false;
    }
    if (rings_[slot].Push(record)) {
      accepted_[slot].fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    dropped_[slot].fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  /**
   * @brief Consumer side: pop the next record, draining all rings round-robin.
   * @param out Receives the record when one is available.
   * @return False when every ring is empty.
   *
   * Single reader only.
   */
  bool TryPop(LogRecord& out) noexcept {
    for (std::uint32_t i = 0; i < kLogProducerCount; ++i) {
      const std::uint32_t slot = (pop_cursor_ + i) % kLogProducerCount;
      if (rings_[slot].Pop(out)) {
        pop_cursor_ = (slot + 1U) % kLogProducerCount;
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Consumer side: pop the next record from one specific producer.
   * @param producer Ring to drain.
   * @param out Receives the record when one is available.
   * @return False when that ring is empty.
   *
   * Used by tests and targeted drains.
   */
  bool TryPop(LogProducer producer, LogRecord& out) noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(producer);
    if (slot >= kLogProducerCount) {
      return false;
    }
    return rings_[slot].Pop(out);
  }

  std::uint32_t Size() const noexcept {
    std::uint32_t total = 0;
    for (std::uint32_t i = 0; i < kLogProducerCount; ++i) {
      total += static_cast<std::uint32_t>(rings_[i].Size());
    }
    return total;
  }

  std::uint32_t Size(LogProducer producer) const noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(producer);
    return (slot >= kLogProducerCount) ? 0U : static_cast<std::uint32_t>(rings_[slot].Size());
  }

  bool Empty() const noexcept {
    for (std::uint32_t i = 0; i < kLogProducerCount; ++i) {
      if (!rings_[i].IsEmpty()) {
        return false;
      }
    }
    return true;
  }

  bool Full(LogProducer producer) const noexcept {
    const std::uint32_t slot = static_cast<std::uint32_t>(producer);
    return (slot < kLogProducerCount) && rings_[slot].IsFull();
  }

  static constexpr std::uint32_t Depth() noexcept { return kLogRingDepth; }

  std::uint64_t Accepted() const noexcept {
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < kLogProducerCount; ++i) {
      total += accepted_[i].load(std::memory_order_relaxed);
    }
    return total;
  }
  std::uint64_t Accepted(LogProducer producer) const noexcept {
    return accepted_[static_cast<std::uint32_t>(producer)].load(std::memory_order_relaxed);
  }

  std::uint64_t Dropped() const noexcept {
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < kLogProducerCount; ++i) {
      total += dropped_[i].load(std::memory_order_relaxed);
    }
    return total;
  }
  std::uint64_t Dropped(LogProducer producer) const noexcept {
    return dropped_[static_cast<std::uint32_t>(producer)].load(std::memory_order_relaxed);
  }

 private:
  std::array<osp::SpscRingbuffer<LogRecord, kLogRingDepth>, kLogProducerCount> rings_{};
  std::array<std::atomic<std::uint64_t>, kLogProducerCount> accepted_{};
  std::array<std::atomic<std::uint64_t>, kLogProducerCount> dropped_{};
  std::uint32_t pop_cursor_ = 0;  // consumer-only
};

}  // namespace picopaste

// picopaste — fixed-capacity lock-free log queues, one per producer thread.
//
// The log writer is the single consumer. Four threads produce records, and an
// SPSC ring tolerates exactly one producer, so each producer owns its own ring
// (the same architecture newosp's async_log uses). The writer drains all rings
// round-robin. No heap and no mutex on any path.
//
// Producers address their ring by a compile-time slot index (LogProducer), not
// by a runtime thread-id lookup: that is allocation-free and makes the origin
// of every record explicit at the call site.

#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include <atomic>

#include "osp/platform.hpp"
#include "osp/spsc_ringbuffer.hpp"

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

// Fill the timestamp fields from the platform clock (ms wall-clock resolution).
void StampNow(LogRecord& record) noexcept;

// Format `fmt` into a record and stamp it. Never allocates; the message is
// truncated to kLogMessageBytes, which is a bounded, self-inflicted loss.
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

  // Producer side: each slot is pushed by exactly one thread. Returns false
  // when that producer's ring is full; the record is dropped and counted.
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

  // Consumer side (single reader only): drain all producer rings round-robin.
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

  // Consumer side, one specific producer (used by tests and targeted drains).
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

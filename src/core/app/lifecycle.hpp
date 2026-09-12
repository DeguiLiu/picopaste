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
 * @file lifecycle.hpp
 * @brief Process lifecycle state machine.
 *
 * Built on newosp's osp/hsm.hpp (StateMachine), per design decision 7.
 * service_hsm.hpp is deliberately not used: HsmService drags in
 * fault_collector.hpp and bus.hpp, neither of which this process needs.
 *
 * Failure visibility is the point of this layer. A dropped channel moves
 * Ready -> Degraded immediately, so the tray can go red without waiting for a
 * user to notice that paste stopped working.
 *
 * The backoff policy is a pure function (BackoffDelayMs) so tests can drive
 * the 5 s -> 60 s ramp and the 3-minute stable reset without real time. The
 * machine's own clock is injectable for the same reason.
 */

#pragma once

#include "osp/hsm.hpp"
#include "osp/vocabulary.hpp"

#include <cstdint>

namespace picopaste {

// Backoff constants from the previous Go supervision loop.
inline constexpr std::uint32_t kBackoffBaseMs = 5000;
inline constexpr std::uint32_t kBackoffMaxMs = 60000;
inline constexpr std::uint64_t kStableResetMs = 180000;  // 3 minutes

/**
 * @brief Pure backoff policy: delay before retry `attempt`, unless the link had
 * been stable for `stable_ms`, in which case the ramp resets to base.
 * @param attempt zero-based count of consecutive failures.
 * @param stable_ms how long the last Ready stayed up.
 * @return The delay to wait before the next attempt, capped at kBackoffMaxMs.
 */
std::uint32_t BackoffDelayMs(std::uint32_t attempt, std::uint64_t stable_ms) noexcept;

enum class LifecycleState : std::uint8_t {
  kInit = 0,
  kConnecting,
  kReady,
  kDegraded,
  kReconnecting,
  kStopping,
  kStopped,
};

enum class LifecycleEvent : std::uint32_t {
  kStart = 1,
  kConnectOk,
  kConnectFail,
  kChannelLost,
  kRetry,
  kStop,
  kStopped,
};

// Tray rendering hint. Ready is green; Degraded and Stopped are red (a dead
// channel is surfaced, not hidden); transitional states are yellow.
enum class TrayHealth : std::uint8_t { kGreen = 0, kYellow, kRed };

class Lifecycle {
 public:
  // Injectable monotonic clock in milliseconds. Must be non-null before Start.
  using ClockFn = osp::FixedFunction<std::uint64_t(), 16>;

  Lifecycle() noexcept;
  Lifecycle(const Lifecycle&) = delete;
  Lifecycle& operator=(const Lifecycle&) = delete;

  /**
   * @brief Replace the default steady clock (tests drive time explicitly).
   * @param clock must be non-null before Start.
   */
  void SetClock(ClockFn clock) noexcept;

  /**
   * @brief Enter the machine (Init) and arm it.
   *
   * Post(kStart) then drives Connecting.
   */
  void Start() noexcept;

  /**
   * @brief Feed one event to the machine.
   * @return false if the machine has not been started.
   */
  bool Post(LifecycleEvent event) noexcept;

  void Stop() noexcept { (void)Post(LifecycleEvent::kStop); }

  /** @brief Current state, or kInit before the machine is started. */
  LifecycleState State() const noexcept;

  /** @brief Stable name for State(), e.g. "Ready". */
  const char* StateName() const noexcept;

  bool IsReady() const noexcept { return LifecycleState::kReady == State(); }
  bool IsStopped() const noexcept { return LifecycleState::kStopped == State(); }

  /** @brief Tray colour for the current state. */
  TrayHealth Health() const noexcept;

  // Delay that the supervision loop should wait before the next attempt.
  std::uint32_t RetryDelayMs() const noexcept { return ctx_.retry_delay_ms; }
  std::uint32_t Attempt() const noexcept { return ctx_.attempt; }
  std::uint64_t StableMs() const noexcept { return ctx_.stable_ms; }

  std::uint32_t ConnectOkCount() const noexcept { return ctx_.connect_ok_count; }
  std::uint32_t ConnectFailCount() const noexcept { return ctx_.connect_fail_count; }
  std::uint32_t ChannelLostCount() const noexcept { return ctx_.channel_lost_count; }

 private:
  static constexpr std::uint32_t kStateCount = 7;

  struct Context {
    Lifecycle* self = nullptr;
    void* machine = nullptr;  // osp::StateMachine<Context, 7>*
    std::int32_t state_index[kStateCount] = {-1, -1, -1, -1, -1, -1, -1};

    std::uint32_t attempt = 0;         // consecutive failed connect attempts
    std::uint64_t ready_since_ms = 0;  // monotonic time Ready was entered
    std::uint64_t stable_ms = 0;       // how long the last Ready stayed up
    std::uint32_t retry_delay_ms = 0;  // delay to wait before next attempt

    std::uint32_t connect_ok_count = 0;
    std::uint32_t connect_fail_count = 0;
    std::uint32_t channel_lost_count = 0;
  };

  using Machine = osp::StateMachine<Context, 7>;

  static osp::TransitionResult HandleInit(Context& ctx, const osp::Event& event) noexcept;
  static osp::TransitionResult HandleConnecting(Context& ctx, const osp::Event& event) noexcept;
  static osp::TransitionResult HandleReady(Context& ctx, const osp::Event& event) noexcept;
  static osp::TransitionResult HandleDegraded(Context& ctx, const osp::Event& event) noexcept;
  static osp::TransitionResult HandleReconnecting(Context& ctx, const osp::Event& event) noexcept;
  static osp::TransitionResult HandleStopping(Context& ctx, const osp::Event& event) noexcept;
  static osp::TransitionResult HandleStopped(Context& ctx, const osp::Event& event) noexcept;

  static void OnReadyEntry(Context& ctx) noexcept;

  static std::int32_t Index(const Context& ctx, LifecycleState state) noexcept {
    return ctx.state_index[static_cast<std::uint32_t>(state)];
  }
  static std::uint32_t RetryDelay(const Context& ctx, std::uint64_t stable_ms) noexcept;

  // Ready -> Degraded, shared by a lost channel and a failed upload. Both mean
  // the link the retry loop is about to rebuild is gone, so both must show red.
  static osp::TransitionResult Degrade(Context& ctx) noexcept;

  static Machine& MachineOf(const Context& ctx) noexcept { return *static_cast<Machine*>(ctx.machine); }
  std::uint64_t Now() const noexcept { return clock_(); }

  Context ctx_{};
  Machine machine_;
  ClockFn clock_;
};

}  // namespace picopaste

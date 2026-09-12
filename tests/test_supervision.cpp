// picopaste — supervision-loop tests.
//
// Lifecycle is a pure state machine, and test_lifecycle.cpp already pins its
// transitions and the BackoffDelayMs arithmetic. This file tests something
// different: that the WIN32 WORKER LOOP drives the machine correctly. The loop
// cannot run on Linux, but the policy it executes can: the loop's only job is
// to post the events below and to arm a waitable timer with RetryDelayMs().
// Driving the same public API here proves the sequence and the delay the loop
// would use, without a message loop. Every state that would set a tray colour on
// screen is asserted through Health().
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "../src/core/app/lifecycle.hpp"

using picopaste::kBackoffBaseMs;
using picopaste::Lifecycle;
using picopaste::LifecycleEvent;
using picopaste::LifecycleState;
using picopaste::TrayHealth;

TEST_CASE("supervision loop: idle channel loss -> Degraded -> backoff -> Reconnecting -> Ready",
          "[supervision]") {
  std::uint64_t now_ms = 1'000'000;
  Lifecycle loop;
  loop.SetClock([&now_ms]() noexcept -> std::uint64_t { return now_ms; });

  // Worker start: Start() posts kStart, Init -> Connecting.
  loop.Start();
  CHECK(loop.Health() == TrayHealth::kYellow);
  loop.Post(LifecycleEvent::kStart);
  REQUIRE(loop.State() == LifecycleState::kConnecting);
  CHECK(loop.Health() == TrayHealth::kYellow);

  // First successful channel open / first successful paste.
  loop.Post(LifecycleEvent::kConnectOk);
  REQUIRE(loop.State() == LifecycleState::kReady);
  CHECK(loop.Health() == TrayHealth::kGreen);

  // The ssh child dies while idle. The loop's WaitForMultipleObjects wakes on
  // the process handle and posts kChannelLost from Ready.
  now_ms += 2000u;  // 2 s of uptime, well under the 3-minute stable reset
  loop.Post(LifecycleEvent::kChannelLost);
  REQUIRE(loop.State() == LifecycleState::kDegraded);
  CHECK(loop.Health() == TrayHealth::kRed);  // a dead channel must be visible
  const std::uint32_t degraded_wait_ms = loop.RetryDelayMs();
  CHECK(degraded_wait_ms == kBackoffBaseMs);  // the loop arms the timer with this

  // The waitable timer elapses -> kRetry -> Reconnecting.
  loop.Post(LifecycleEvent::kRetry);
  REQUIRE(loop.State() == LifecycleState::kReconnecting);
  CHECK(loop.Health() == TrayHealth::kYellow);

  // First reconnect attempt fails: Reconnecting self-transitions and the loop
  // re-arms the timer from RetryDelayMs().
  loop.Post(LifecycleEvent::kConnectFail);
  REQUIRE(loop.State() == LifecycleState::kReconnecting);
  CHECK(loop.Attempt() == 1u);
  const std::uint32_t retry1_ms = loop.RetryDelayMs();
  CHECK(retry1_ms == kBackoffBaseMs);

  // Second failure: the ramp has actually grown.
  loop.Post(LifecycleEvent::kConnectFail);
  CHECK(loop.Attempt() == 2u);
  const std::uint32_t retry2_ms = loop.RetryDelayMs();
  CHECK(retry2_ms == (2u * kBackoffBaseMs));

  // Next timer tick reconnects successfully. kRetry is a no-op in Reconnecting
  // (the machine has no handler for it there); it models the timer firing.
  loop.Post(LifecycleEvent::kRetry);
  loop.Post(LifecycleEvent::kConnectOk);
  REQUIRE(loop.State() == LifecycleState::kReady);
  CHECK(loop.Health() == TrayHealth::kGreen);

  // Shutdown: Stop() -> Stopping -> Stopped.
  loop.Stop();
  REQUIRE(loop.State() == LifecycleState::kStopping);
  CHECK(loop.Health() == TrayHealth::kYellow);
  loop.Post(LifecycleEvent::kStopped);
  REQUIRE(loop.State() == LifecycleState::kStopped);
  CHECK(loop.Health() == TrayHealth::kRed);
}

TEST_CASE("supervision loop: a 3-minute-stable link resets the backoff the timer arms",
          "[supervision]") {
  std::uint64_t now_ms = 5'000'000;
  Lifecycle loop;
  loop.SetClock([&now_ms]() noexcept -> std::uint64_t { return now_ms; });
  loop.Start();
  loop.Post(LifecycleEvent::kStart);

  // Six consecutive spawn failures ramp to the ceiling.
  for (std::int32_t i = 0; i < 6; ++i) {
    loop.Post(LifecycleEvent::kConnectFail);
  }
  REQUIRE(loop.State() == LifecycleState::kReconnecting);
  CHECK(loop.Attempt() == 6u);
  CHECK(loop.RetryDelayMs() == 60000u);

  // A link that stays Ready for three minutes earns a fresh ramp.
  loop.Post(LifecycleEvent::kConnectOk);
  now_ms += 180000u;
  loop.Post(LifecycleEvent::kChannelLost);
  REQUIRE(loop.State() == LifecycleState::kDegraded);
  CHECK(loop.Health() == TrayHealth::kRed);
  CHECK(loop.Attempt() == 0u);
  CHECK(loop.RetryDelayMs() == kBackoffBaseMs);  // the timer waits the base again
}

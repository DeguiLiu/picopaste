// picopaste — lifecycle state machine and backoff policy tests.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "../src/core/app/lifecycle.hpp"

using picopaste::Lifecycle;
using picopaste::LifecycleEvent;
using picopaste::LifecycleState;
using picopaste::TrayHealth;

namespace {

// Drive the machine to Ready from a fresh start.
void DriveToReady(Lifecycle& lc) {
  lc.Start();
  lc.Post(LifecycleEvent::kStart);
  lc.Post(LifecycleEvent::kConnectOk);
}

}  // namespace

TEST_CASE("Backoff policy ramps 5s to a 60s ceiling", "[lifecycle]") {
  CHECK(picopaste::BackoffDelayMs(0, 0) == 5000U);
  CHECK(picopaste::BackoffDelayMs(1, 0) == 10000U);
  CHECK(picopaste::BackoffDelayMs(2, 0) == 20000U);
  CHECK(picopaste::BackoffDelayMs(3, 0) == 40000U);
  CHECK(picopaste::BackoffDelayMs(4, 0) == 60000U);
  CHECK(picopaste::BackoffDelayMs(5, 0) == 60000U);
  CHECK(picopaste::BackoffDelayMs(99, 0) == 60000U);
}

TEST_CASE("Backoff resets to base only after three stable minutes", "[lifecycle]") {
  CHECK(picopaste::BackoffDelayMs(5, 179999U) == 60000U);
  CHECK(picopaste::BackoffDelayMs(5, 180000U) == 5000U);
  CHECK(picopaste::BackoffDelayMs(0, 180000U) == 5000U);
}

TEST_CASE("Lifecycle walks the nominal transition path", "[lifecycle]") {
  Lifecycle lc;
  lc.Start();

  CHECK(lc.State() == LifecycleState::kInit);
  CHECK(std::string(lc.StateName()) == "Init");
  CHECK(lc.Health() == TrayHealth::kYellow);

  CHECK(lc.Post(LifecycleEvent::kStart));
  CHECK(lc.State() == LifecycleState::kConnecting);

  lc.Post(LifecycleEvent::kConnectOk);
  CHECK(lc.State() == LifecycleState::kReady);
  CHECK(lc.IsReady());
  CHECK(lc.Health() == TrayHealth::kGreen);
  CHECK(lc.ConnectOkCount() == 1U);

  lc.Post(LifecycleEvent::kChannelLost);
  CHECK(lc.State() == LifecycleState::kDegraded);
  CHECK(lc.Health() == TrayHealth::kRed);
  CHECK(lc.ChannelLostCount() == 1U);

  lc.Post(LifecycleEvent::kRetry);
  CHECK(lc.State() == LifecycleState::kReconnecting);
  CHECK(lc.Health() == TrayHealth::kYellow);

  lc.Post(LifecycleEvent::kConnectOk);
  CHECK(lc.State() == LifecycleState::kReady);

  lc.Stop();
  CHECK(lc.State() == LifecycleState::kStopping);

  lc.Post(LifecycleEvent::kStopped);
  CHECK(lc.State() == LifecycleState::kStopped);
  CHECK(lc.IsStopped());
  CHECK(lc.Health() == TrayHealth::kRed);
}

TEST_CASE("Stop is reachable from every state", "[lifecycle]") {
  SECTION("from Connecting") {
    Lifecycle lc;
    lc.Start();
    lc.Post(LifecycleEvent::kStart);
    lc.Stop();
    CHECK(lc.State() == LifecycleState::kStopping);
  }
  SECTION("from Ready") {
    Lifecycle lc;
    DriveToReady(lc);
    lc.Stop();
    CHECK(lc.State() == LifecycleState::kStopping);
  }
  SECTION("from Degraded") {
    Lifecycle lc;
    DriveToReady(lc);
    lc.Post(LifecycleEvent::kChannelLost);
    lc.Stop();
    CHECK(lc.State() == LifecycleState::kStopping);
  }
  SECTION("from Reconnecting") {
    Lifecycle lc;
    lc.Start();
    lc.Post(LifecycleEvent::kStart);
    lc.Post(LifecycleEvent::kConnectFail);
    lc.Stop();
    CHECK(lc.State() == LifecycleState::kStopping);
  }
}

TEST_CASE("Repeated connect failures grow the retry delay", "[lifecycle]") {
  Lifecycle lc;
  lc.Start();
  lc.Post(LifecycleEvent::kStart);

  lc.Post(LifecycleEvent::kConnectFail);
  CHECK(lc.State() == LifecycleState::kReconnecting);
  CHECK(lc.Attempt() == 1U);
  CHECK(lc.RetryDelayMs() == 5000U);

  lc.Post(LifecycleEvent::kConnectFail);
  CHECK(lc.Attempt() == 2U);
  CHECK(lc.RetryDelayMs() == 10000U);

  lc.Post(LifecycleEvent::kConnectFail);
  CHECK(lc.RetryDelayMs() == 20000U);

  lc.Post(LifecycleEvent::kConnectFail);
  CHECK(lc.RetryDelayMs() == 40000U);

  lc.Post(LifecycleEvent::kConnectFail);
  CHECK(lc.RetryDelayMs() == 60000U);

  lc.Post(LifecycleEvent::kConnectFail);
  CHECK(lc.RetryDelayMs() == 60000U);  // ceiling
  CHECK(lc.ConnectFailCount() == 6U);

  lc.Post(LifecycleEvent::kConnectOk);
  CHECK(lc.State() == LifecycleState::kReady);
}

TEST_CASE("The 3-minute stable mark resets the backoff", "[lifecycle]") {
  std::uint64_t now_ms = 0;
  Lifecycle lc;
  lc.SetClock([&now_ms]() noexcept -> std::uint64_t { return now_ms; });
  lc.Start();
  lc.Post(LifecycleEvent::kStart);

  // Six failures: attempt = 6, delay pinned at the ceiling.
  for (std::int32_t i = 0; i < 6; ++i) {
    lc.Post(LifecycleEvent::kConnectFail);
  }
  CHECK(lc.Attempt() == 6U);
  CHECK(lc.RetryDelayMs() == 60000U);

  // First Ready lasts just under three minutes: ramp is NOT reset.
  now_ms = 0;
  lc.Post(LifecycleEvent::kConnectOk);
  now_ms = 179999U;
  lc.Post(LifecycleEvent::kChannelLost);
  CHECK(lc.State() == LifecycleState::kDegraded);
  CHECK(lc.Attempt() == 6U);
  CHECK(lc.RetryDelayMs() == 60000U);

  // Second Ready lasts exactly three minutes: ramp resets to base.
  lc.Post(LifecycleEvent::kRetry);
  now_ms = 179999U;
  lc.Post(LifecycleEvent::kConnectOk);
  now_ms = 179999U + 180000U;
  lc.Post(LifecycleEvent::kChannelLost);
  CHECK(lc.Attempt() == 0U);
  CHECK(lc.StableMs() == 180000U);
  CHECK(lc.RetryDelayMs() == 5000U);
}

TEST_CASE("A failed upload while Ready degrades and shows red", "[lifecycle]") {
  // The worker loop drops the channel and posts kConnectFail for any upload
  // failure, including a bounded upload's kUploadTimeout. If Ready ignored the
  // event the tray would stay green after a real failure -- the silent-failure
  // mode this project exists to remove.
  Lifecycle lc;
  DriveToReady(lc);
  REQUIRE(lc.State() == LifecycleState::kReady);
  CHECK(lc.Health() == TrayHealth::kGreen);

  lc.Post(LifecycleEvent::kConnectFail);
  CHECK(lc.State() == LifecycleState::kDegraded);
  CHECK(lc.Health() == TrayHealth::kRed);
  CHECK(lc.ConnectFailCount() == 1U);
  CHECK(lc.RetryDelayMs() == picopaste::kBackoffBaseMs);  // the loop arms a fresh retry

  // Recovery is the normal degraded path: kRetry -> Reconnecting -> kConnectOk.
  lc.Post(LifecycleEvent::kRetry);
  CHECK(lc.State() == LifecycleState::kReconnecting);
  lc.Post(LifecycleEvent::kConnectOk);
  CHECK(lc.State() == LifecycleState::kReady);
  CHECK(lc.Health() == TrayHealth::kGreen);
}

TEST_CASE("Post before Start is refused", "[lifecycle]") {
  Lifecycle lc;
  CHECK_FALSE(lc.Post(LifecycleEvent::kStart));
  CHECK(lc.State() == LifecycleState::kInit);
}

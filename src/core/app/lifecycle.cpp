// picopaste — lifecycle state machine implementation.

#include "lifecycle.hpp"

namespace picopaste {
namespace {

constexpr const char* kStateNames[7] = {"Init",     "Connecting", "Ready", "Degraded",
                                        "Reconnecting", "Stopping", "Stopped"};

}  // namespace

std::uint32_t Lifecycle::RetryDelay(const Context& ctx, std::uint64_t stable_ms) noexcept {
  // `attempt` counts consecutive failures; the first failure waits one base
  // interval.
  const std::uint32_t index = (0 == ctx.attempt) ? 0U : (ctx.attempt - 1U);
  return BackoffDelayMs(index, stable_ms);
}

std::uint32_t BackoffDelayMs(std::uint32_t attempt, std::uint64_t stable_ms) noexcept {
  if (stable_ms >= kStableResetMs) {
    return kBackoffBaseMs;
  }
  std::uint64_t delay = kBackoffBaseMs;
  for (std::uint32_t i = 0; i < attempt; ++i) {
    if (delay >= kBackoffMaxMs) {
      break;
    }
    delay *= 2U;
  }
  if (delay > kBackoffMaxMs) {
    delay = kBackoffMaxMs;
  }
  return static_cast<std::uint32_t>(delay);
}

Lifecycle::Lifecycle() noexcept : ctx_{}, machine_(ctx_), clock_() {
  ctx_.self = this;
  ctx_.machine = &machine_;

  const auto add = [this](LifecycleState state, const char* name,
                          osp::TransitionResult (*handler)(Context&, const osp::Event&),
                          void (*entry)(Context&)) {
    const osp::StateConfig<Context> config{name, -1, handler, entry, nullptr, nullptr};
    ctx_.state_index[static_cast<std::uint32_t>(state)] = machine_.AddState(config);
  };
  add(LifecycleState::kInit, kStateNames[0], &Lifecycle::HandleInit, nullptr);
  add(LifecycleState::kConnecting, kStateNames[1], &Lifecycle::HandleConnecting, nullptr);
  add(LifecycleState::kReady, kStateNames[2], &Lifecycle::HandleReady, &Lifecycle::OnReadyEntry);
  add(LifecycleState::kDegraded, kStateNames[3], &Lifecycle::HandleDegraded, nullptr);
  add(LifecycleState::kReconnecting, kStateNames[4], &Lifecycle::HandleReconnecting, nullptr);
  add(LifecycleState::kStopping, kStateNames[5], &Lifecycle::HandleStopping, nullptr);
  add(LifecycleState::kStopped, kStateNames[6], &Lifecycle::HandleStopped, nullptr);

  machine_.SetInitialState(ctx_.state_index[static_cast<std::uint32_t>(LifecycleState::kInit)]);

  clock_ = []() noexcept -> std::uint64_t { return osp::SteadyNowUs() / 1000ULL; };
}

void Lifecycle::SetClock(ClockFn clock) noexcept { clock_ = static_cast<ClockFn&&>(clock); }

void Lifecycle::Start() noexcept {
  if (machine_.IsStarted()) {
    return;
  }
  machine_.Start();
}

bool Lifecycle::Post(LifecycleEvent event) noexcept {
  if (!machine_.IsStarted()) {
    return false;
  }
  machine_.Dispatch(osp::Event{static_cast<std::uint32_t>(event), nullptr});
  return true;
}

LifecycleState Lifecycle::State() const noexcept {
  const std::int32_t current = machine_.CurrentState();
  for (std::uint32_t i = 0; i < kStateCount; ++i) {
    if (ctx_.state_index[i] == current) {
      return static_cast<LifecycleState>(i);
    }
  }
  return LifecycleState::kInit;
}

const char* Lifecycle::StateName() const noexcept { return kStateNames[static_cast<std::uint32_t>(State())]; }

TrayHealth Lifecycle::Health() const noexcept {
  switch (State()) {
    case LifecycleState::kReady:
      return TrayHealth::kGreen;
    case LifecycleState::kDegraded:
    case LifecycleState::kStopped:
      return TrayHealth::kRed;
    case LifecycleState::kInit:
    case LifecycleState::kConnecting:
    case LifecycleState::kReconnecting:
    case LifecycleState::kStopping:
    default:
      return TrayHealth::kYellow;
  }
}

void Lifecycle::OnReadyEntry(Context& ctx) noexcept {
  ctx.ready_since_ms = ctx.self->Now();
  ctx.stable_ms = 0;
}

osp::TransitionResult Lifecycle::HandleInit(Context& ctx, const osp::Event& event) noexcept {
  switch (static_cast<LifecycleEvent>(event.id)) {
    case LifecycleEvent::kStart:
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kConnecting));
    case LifecycleEvent::kStop:
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kStopping));
    default:
      return osp::TransitionResult::kUnhandled;
  }
}

osp::TransitionResult Lifecycle::HandleConnecting(Context& ctx, const osp::Event& event) noexcept {
  switch (static_cast<LifecycleEvent>(event.id)) {
    case LifecycleEvent::kConnectOk:
      ++ctx.connect_ok_count;
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kReady));
    case LifecycleEvent::kConnectFail:
      ++ctx.connect_fail_count;
      ++ctx.attempt;
      ctx.retry_delay_ms = RetryDelay(ctx, ctx.stable_ms);
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kReconnecting));
    case LifecycleEvent::kStop:
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kStopping));
    default:
      return osp::TransitionResult::kUnhandled;
  }
}

osp::TransitionResult Lifecycle::HandleReady(Context& ctx, const osp::Event& event) noexcept {
  switch (static_cast<LifecycleEvent>(event.id)) {
    case LifecycleEvent::kChannelLost:
      ++ctx.channel_lost_count;
      ctx.stable_ms = ctx.self->Now() - ctx.ready_since_ms;
      if (ctx.stable_ms >= kStableResetMs) {
        ctx.attempt = 0;  // the link earned a fresh ramp
      }
      ctx.retry_delay_ms = RetryDelay(ctx, ctx.stable_ms);
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kDegraded));
    case LifecycleEvent::kStop:
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kStopping));
    default:
      return osp::TransitionResult::kUnhandled;
  }
}

osp::TransitionResult Lifecycle::HandleDegraded(Context& ctx, const osp::Event& event) noexcept {
  switch (static_cast<LifecycleEvent>(event.id)) {
    case LifecycleEvent::kRetry:
      ctx.retry_delay_ms = RetryDelay(ctx, ctx.stable_ms);
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kReconnecting));
    case LifecycleEvent::kStop:
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kStopping));
    default:
      return osp::TransitionResult::kUnhandled;
  }
}

osp::TransitionResult Lifecycle::HandleReconnecting(Context& ctx, const osp::Event& event) noexcept {
  switch (static_cast<LifecycleEvent>(event.id)) {
    case LifecycleEvent::kConnectOk:
      ++ctx.connect_ok_count;
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kReady));
    case LifecycleEvent::kConnectFail:
      ++ctx.connect_fail_count;
      ++ctx.attempt;
      ctx.retry_delay_ms = RetryDelay(ctx, 0);
      // Self-transition: re-run entry so a supervisory timer can re-arm.
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kReconnecting));
    case LifecycleEvent::kStop:
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kStopping));
    default:
      return osp::TransitionResult::kUnhandled;
  }
}

osp::TransitionResult Lifecycle::HandleStopping(Context& ctx, const osp::Event& event) noexcept {
  switch (static_cast<LifecycleEvent>(event.id)) {
    case LifecycleEvent::kStopped:
      return MachineOf(ctx).RequestTransition(Index(ctx, LifecycleState::kStopped));
    case LifecycleEvent::kStop:
      return osp::TransitionResult::kHandled;
    default:
      return osp::TransitionResult::kUnhandled;
  }
}

osp::TransitionResult Lifecycle::HandleStopped(Context& /*ctx*/, const osp::Event& event) noexcept {
  switch (static_cast<LifecycleEvent>(event.id)) {
    case LifecycleEvent::kStopped:
    case LifecycleEvent::kStop:
      return osp::TransitionResult::kHandled;  // terminal
    default:
      return osp::TransitionResult::kUnhandled;
  }
}

}  // namespace picopaste

// picopaste — upload-and-paste pipeline implementation (see the header).

#include "upload_pipeline.hpp"

#include "osp/platform.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace picopaste {
namespace {

// RAII for the in-flight flag: acquire is the compare-and-swap in Run, release
// is this destructor, so every exit path clears the flag exactly once.
class FlightGuard final {
 public:
  explicit FlightGuard(std::atomic<bool>& flag) noexcept : flag_(&flag) {}
  ~FlightGuard() noexcept { flag_->store(false, std::memory_order_release); }

  FlightGuard(const FlightGuard&) = delete;
  FlightGuard& operator=(const FlightGuard&) = delete;

 private:
  std::atomic<bool>* flag_;
};

// RAII for the capture. It reads the token live from `captured` rather than
// copying it at construction: the guard is created before capture() runs, and
// the token only exists once capture() has succeeded.
class CaptureGuard final {
 public:
  CaptureGuard(const ClipboardOps& clip, const CapturedClip& captured) noexcept : clip_(&clip), captured_(&captured) {}
  ~CaptureGuard() noexcept {
    if ((nullptr != captured_->restore_token) && (nullptr != clip_->release)) {
      clip_->release(clip_->ctx, captured_->restore_token);
    }
  }

  CaptureGuard(const CaptureGuard&) = delete;
  CaptureGuard& operator=(const CaptureGuard&) = delete;

 private:
  const ClipboardOps* clip_;
  const CapturedClip* captured_;
};

// Resolve configuration's remote_dir to an absolute path. A leading "~/" is
// expanded by asking the SFTP subsystem for its own working directory
// (REALPATH "." is the user's home on OpenSSH) — no shell, no HOME probe, no
// banner to filter. An already-absolute path is used verbatim.
Status ResolveRemoteDir(sftp::Client& client, const char* remote_dir, char* buf, std::size_t cap) noexcept {
  if ((nullptr == remote_dir) || ('\0' == remote_dir[0])) {
    return Status::error(Error::kMkdirFailed);
  }
  std::int32_t written = -1;
  if (('~' == remote_dir[0]) && ('/' == remote_dir[1])) {
    const auto home = client.Realpath(".");
    if (!home) {
      return Status::error(Error::kRealpathFailed);
    }
    written = std::snprintf(buf, cap, "%s/%s", home.value().c_str(), remote_dir + 2u);
  } else {
    written = std::snprintf(buf, cap, "%s", remote_dir);
  }
  if ((0 > written) || (static_cast<std::size_t>(written) >= cap)) {
    return Status::error(Error::kMkdirFailed);  // would overflow the remote path cap
  }
  return Status::success();
}

// Build this project's own upload name: clip-YYYYMMDD-HHMMSS-<hex>.png. The
// suffix is a mix of the timestamp and a counter so two uploads in the same
// second do not collide. gmtime's shared buffer is safe here because Run() is
// single-flight by construction.
void MakeObjectName(std::uint64_t now_unix, std::uint32_t suffix, char* out, std::size_t cap) noexcept {
  const std::time_t when = static_cast<std::time_t>(now_unix);
  const std::tm* parts = std::gmtime(&when);
  std::int32_t year = 1970;
  std::int32_t month = 1;
  std::int32_t day = 1;
  std::int32_t hour = 0;
  std::int32_t minute = 0;
  std::int32_t second = 0;
  if (nullptr != parts) {
    year = parts->tm_year + 1900;
    month = parts->tm_mon + 1;
    day = parts->tm_mday;
    hour = parts->tm_hour;
    minute = parts->tm_min;
    second = parts->tm_sec;
  }
  (void)std::snprintf(out, cap, "clip-%04d%02d%02d-%02d%02d%02d-%08x.png", year, month, day, hour, minute, second,
                      suffix);
}

}  // namespace

UploadPipeline::UploadPipeline(const Config& cfg) noexcept : cfg_(cfg) {
  retention_.keep_newest = kDefaultKeepNewest;
  retention_.max_age_seconds = kDefaultMaxAgeSeconds;
  clock_ = []() noexcept -> std::uint64_t { return osp::SteadyNowUs() / 1000ULL; };
}

void UploadPipeline::SetClock(ClockFn clock) noexcept {
  clock_ = static_cast<ClockFn&&>(clock);
}

bool UploadPipeline::UploadOverdue() noexcept {
  const std::uint32_t generation = active_generation_.load(std::memory_order_acquire);
  if (0u == generation) {
    return false;  // nothing in flight
  }
  if (DeadlinePassed(generation)) {
    return true;  // already latched; the run has not retired yet
  }
  if (0u == cfg_.upload_timeout_ms) {
    return false;  // deadline disabled
  }
  // The generation is already visible; its release store also published the
  // start time and the armed flag read below.
  const std::uint64_t start_ms = flight_start_ms_.load(std::memory_order_relaxed);
  if (!flight_armed_.load(std::memory_order_relaxed)) {
    return false;  // the flag was won but the flight has not armed yet
  }
  if ((Now() - start_ms) < static_cast<std::uint64_t>(cfg_.upload_timeout_ms)) {
    return false;
  }
  // Re-read: the run may have retired between the load above and here, in which
  // case this deadline belongs to no live flight and must not be counted.
  if (active_generation_.load(std::memory_order_acquire) != generation) {
    return false;
  }
  expired_generation_.store(generation, std::memory_order_release);
  upload_timeout_count_.fetch_add(1u, std::memory_order_relaxed);
  return true;
}

Status UploadPipeline::CaptureLocal(const ClipboardOps& clip, CapturedClip& out) noexcept {
  out = CapturedClip{};
  if (!clip.valid()) {
    return Status::error(Error::kClipboardOpenFailed);
  }
  const Error e = clip.capture(clip.ctx, cfg_, out);
  if (Error::kOk != e) {
    return Status::error(e);
  }
  if ((false == out.captured) || out.local_path.empty() || (0u == out.bytes)) {
    return Status::error(Error::kNoImageInClipboard);
  }
  return Status::success();
}

Status UploadPipeline::EnsureRemoteDir(sftp::Client& client, sftp::Path& out) noexcept {
  char buf[sftp::kMaxPathBytes];
  Status s = ResolveRemoteDir(client, cfg_.remote_dir.c_str(), buf, sizeof(buf));
  if (s) {
    s = client.MkdirAll(buf);
    if (!s) {
      // Collapse the client's specific code into the documented mkdir failure:
      // the caller's recovery is the same either way.
      s = Status::error(Error::kMkdirFailed);
    }
  }
  if (!s) {
    return s;
  }
  out.assign(osp::TruncateToCapacity, buf);
  return Status::success();
}

Status UploadPipeline::UploadAndVerify(sftp::Client& client, const CapturedClip& capture, const sftp::Path& dir,
                                       sftp::Path& published, std::uint64_t now_unix) noexcept {
  const std::uint32_t suffix = (static_cast<std::uint32_t>(now_unix) * 2654435761u) ^ (counter_ * 2246822519u);
  ++counter_;
  char name[96];
  MakeObjectName(now_unix, suffix, name, sizeof(name));

  char tmp_buf[sftp::kMaxPathBytes];
  char final_buf[sftp::kMaxPathBytes];
  const std::int32_t t = std::snprintf(tmp_buf, sizeof(tmp_buf), "%s/.tmp-%s", dir.c_str(), name);
  const std::int32_t f = std::snprintf(final_buf, sizeof(final_buf), "%s/%s", dir.c_str(), name);
  if ((0 > t) || (static_cast<std::size_t>(t) >= sizeof(tmp_buf)) || (0 > f) ||
      (static_cast<std::size_t>(f) >= sizeof(final_buf))) {
    return Status::error(Error::kOpenFailed);  // remote path cap exceeded
  }

  // Upload under a temp name and publish by RENAME only after the size check.
  // Until then the file is invisible to the retention pattern and to any
  // consumer watching the directory.
  Status s = client.UploadFile(tmp_buf, capture.local_path.c_str());
  if (s) {
    const auto remote = client.StatSize(tmp_buf);
    if (remote) {
      // This is the re-read the design demands: compare what the server reports
      // with what we captured, not with what we hope we sent.
      s = (remote.value() == capture.bytes) ? Status::success() : Status::error(Error::kSizeMismatch);
    } else {
      s = Status::error(Error::kStatFailed);
    }
  }
  if (s) {
    s = client.Rename(tmp_buf, final_buf);
  }
  if (!s) {
    // Leave no half-published temp behind; a vanished REMOVE is success.
    (void)client.Remove(tmp_buf);
    return s;
  }
  published.assign(osp::TruncateToCapacity, final_buf);
  return Status::success();
}

Status UploadPipeline::Publish(const ClipboardOps& clip, const InjectOps& inject, const CapturedClip& capture,
                               const char* remote_path, bool& restored) noexcept {
  restored = false;
  if (!inject.valid()) {
    return Status::error(Error::kSendInputRejected);
  }
  const Error set = clip.set_text(clip.ctx, remote_path);
  if (Error::kOk != set) {
    return Status::error(set);
  }
  const Error injected = inject.paste(inject.ctx, cfg_.delay_ms);
  // Restore even when the chord failed: after a focus change the freshly-set
  // path is the wrong thing to leave on the user's clipboard. A restore failure
  // must not turn a delivered (or withheld) paste into a different error, but it
  // must not be reported as a restore either.
  if (cfg_.restore_clipboard && capture.captured && (nullptr != capture.restore_token)) {
    restored = (Error::kOk == clip.restore(clip.ctx, capture.restore_token));
  }
  if (Error::kOk != injected) {
    return Status::error(injected);
  }
  return Status::success();
}

void UploadPipeline::Prune(sftp::Client& client, const sftp::Path& dir, std::uint64_t now_unix,
                           UploadReport& report) noexcept {
  if ((0u == retention_.keep_newest) && (0u == retention_.max_age_seconds)) {
    return;
  }
  sftp::DirOps dir_ops(client);
  const auto cleaned = dir_ops.Cleanup(dir.c_str(), now_unix, retention_);
  if (!cleaned) {
    report.prune_failed = true;
    return;
  }
  report.pruned = true;
  report.pruned_files = cleaned.value().removed;
}

Result<UploadReport> UploadPipeline::Run(sftp::Client& client, const ClipboardOps& clipboard, const InjectOps& inject,
                                         std::uint64_t now_unix) noexcept {
  // Single-flight. The exchange returns the previous value: false -> we won the
  // slot; true -> another run owns it and this trigger is refused with kBusy.
  if (in_flight_.exchange(true, std::memory_order_acq_rel)) {
    return Result<UploadReport>::error(Error::kBusy);
  }
  const FlightGuard flight(in_flight_);

  // Arm this flight's deadline once the flag is won, so a refused trigger
  // cannot disturb the running one. The generation keys the start time and the
  // expiry latch, so no state from the previous upload can end this one.
  const std::uint32_t generation = flight_generation_.fetch_add(1u, std::memory_order_relaxed) + 1u;
  flight_start_ms_.store(Now(), std::memory_order_relaxed);
  flight_armed_.store(true, std::memory_order_relaxed);
  active_generation_.store(generation, std::memory_order_release);

  CapturedClip captured{};
  const CaptureGuard cleanup(clipboard, captured);

  sftp::Path dir{};
  sftp::Path remote{};
  bool restored = false;

  // Chain the steps so only the first failure is reported and no later step runs
  // after it. This is the ordering guarantee: nothing reaches the clipboard
  // unless capture, mkdir, upload, size verification and rename all succeeded.
  Status step = CaptureLocal(clipboard, captured);
  if (step && DeadlinePassed(generation)) {
    step = Status::error(Error::kUploadTimeout);
  }
  if (step) {
    step = EnsureRemoteDir(client, dir);
  }
  if (step && DeadlinePassed(generation)) {
    step = Status::error(Error::kUploadTimeout);
  }
  if (step) {
    step = UploadAndVerify(client, captured, dir, remote, now_unix);
  }
  if (step && DeadlinePassed(generation)) {
    step = Status::error(Error::kUploadTimeout);
  }
  if (step) {
    step = Publish(clipboard, inject, captured, remote.c_str(), restored);
  }
  if (step && DeadlinePassed(generation)) {
    step = Status::error(Error::kUploadTimeout);
  }

  // The deadline is enforced at these boundaries: an upload that expired while
  // a step was blocked must not continue into the next one, even if the step it
  // was stuck in eventually completed. The last check keeps the returned code
  // consistent with the latch even when expiry lands inside Publish. Retire the
  // flight before returning so the next run starts with no state from this one;
  // FlightGuard then releases the flag on this exit like every other.
  if (!step) {
    flight_armed_.store(false, std::memory_order_relaxed);
    active_generation_.store(0u, std::memory_order_release);
    return Result<UploadReport>::error(step.get_error());
  }

  UploadReport report{};
  report.remote_path.assign(osp::TruncateToCapacity, remote.c_str());
  report.bytes = captured.bytes;
  report.clipboard_restored = restored;
  // Prune is the last blocking step, so the deadline must cover it too: retire
  // only once it returns, then read the latch. Retiring first is what makes the
  // read authoritative -- once active_generation_ is 0 no new latch can land,
  // so DeadlinePassed sees every expiry that beat retirement, including one that
  // arrived while Prune was blocked against a silent peer.
  Prune(client, dir, now_unix, report);
  flight_armed_.store(false, std::memory_order_relaxed);
  active_generation_.store(0u, std::memory_order_release);
  if (DeadlinePassed(generation)) {
    return Result<UploadReport>::error(Error::kUploadTimeout);
  }
  return Result<UploadReport>::success(report);
}

}  // namespace picopaste

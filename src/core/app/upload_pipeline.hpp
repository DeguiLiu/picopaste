// picopaste — platform-neutral upload-and-paste pipeline.
//
// This file holds the product's central promise. One hotkey press must: capture
// the clipboard image to a local temp file, upload it over the one SFTP client,
// prove the remote byte count matches, publish the remote path to the clipboard,
// inject the paste chord, and prune the upload directory. The ORDER is the
// contract. Every step that can fail runs before anything is written to the
// clipboard or typed into a terminal, so a failed paste is a visible failure and
// never a silent no-op.
//
// The platform never enters this translation unit. Clipboard capture, clipboard
// text publication, clipboard restore and keystroke injection are injected as
// function-pointer tables plus an opaque ctx — the same shape as
// sftp::ByteStream, for the same stated reason (newosp ranks function-pointer +
// ctx above virtual dispatch, and it keeps this core in a .cpp instead of a
// template instantiated at every call site).
//
// The SFTP channel itself is passed in as an already-INIT'ed Client&: spawning
// `ssh -s <host> sftp` is a platform concern, and the Client is deliberately
// non-movable and ~128 KB, so its lifetime belongs to the caller (the design
// requires a long-lived instance, never a 256 KB worker stack frame).
#pragma once

#include "../sftp/dir_ops.hpp"  // sftp::RetentionPolicy / DirOps
#include "osp/vocabulary.hpp"
#include "picopaste/config.hpp"
#include "picopaste/error.hpp"
#include "picopaste/sftp/client.hpp"

#include <cstdint>

#include <atomic>

namespace picopaste {

// Longest local temp-file path the pipeline carries. Kept in step with the
// win32 capture buffer (clipboard.hpp kCapturedPathBytes) without pulling a
// platform header into the core.
inline constexpr std::uint32_t kLocalPathBytes = 512;

// What a successful capture produced. `restore_token` is opaque to this layer:
// it identifies the clipboard ops' own snapshot and is handed straight back to
// restore()/release(). The ops own it; this layer must not interpret it.
struct CapturedClip {
  osp::FixedString<kLocalPathBytes> local_path{};
  std::uint64_t bytes = 0;
  void* restore_token = nullptr;
  bool captured = false;
};

// Clipboard dependency table.
//
// capture() MUST close the clipboard before it returns: holding it open across a
// multi-second upload blocks every other application and lets the source DIB go
// stale. That is why the design chose the temp-file route, and it is the one
// contract a platform implementation must not break.
//
// All four operations are noexcept and none may throw across this boundary.
struct ClipboardOps {
  void* ctx = nullptr;

  // Capture the current clipboard image into a local temp file and describe it
  // in `out`. kNoImageInClipboard when there is nothing to paste.
  Error (*capture)(void* ctx, const Config& cfg, CapturedClip& out) noexcept = nullptr;

  // Put UTF-8 text (the verified remote path) on the clipboard. Called only
  // after every upload step has succeeded.
  Error (*set_text)(void* ctx, const char* utf8) noexcept = nullptr;

  // Best-effort restore of the image captured earlier (cfgs.restore_clipboard).
  Error (*restore)(void* ctx, void* token) noexcept = nullptr;

  // Release the capture's resources (temp file). Must be idempotent.
  void (*release)(void* ctx, void* token) noexcept = nullptr;

  bool valid() const noexcept {
    return (nullptr != ctx) && (nullptr != capture) && (nullptr != set_text) && (nullptr != restore) &&
           (nullptr != release);
  }
};

// Keystroke dependency table. paste() must verify that every event it asked for
// was actually inserted and report a short insert as an error: a best-effort
// "sent" is exactly the lie this project exists to remove (upstream issue #140).
struct InjectOps {
  void* ctx = nullptr;

  // Wait delay_ms, re-check the focus baseline the platform captured when the
  // hotkey fired, and send the chord. Returns kFocusChanged (no key sent) or
  // kSendInputRejected (short insert).
  Error (*paste)(void* ctx, std::uint32_t delay_ms) noexcept = nullptr;

  bool valid() const noexcept { return (nullptr != ctx) && (nullptr != paste); }
};

// Outcome of a successful run. Only produced when the remote path was published
// AND the chord was accepted, so a caller may treat its presence as "delivered".
struct UploadReport {
  osp::FixedString<sftp::kMaxPathBytes> remote_path{};
  std::uint64_t bytes = 0;
  bool clipboard_restored = false;
  // Retention is housekeeping that runs after delivery; a cleanup failure is
  // reported here but does not turn a delivered paste into a failure.
  bool pruned = false;
  bool prune_failed = false;
  std::uint32_t pruned_files = 0;
};

class UploadPipeline final {
 public:
  // Default retention: keep the newest 50 uploads, drop anything older than a
  // week. Zero in a policy field disables that rule.
  static constexpr std::uint32_t kDefaultKeepNewest = 50u;
  static constexpr std::uint64_t kDefaultMaxAgeSeconds = 7ull * 24ull * 3600ull;

  explicit UploadPipeline(const Config& cfg) noexcept;
  UploadPipeline(const UploadPipeline&) = delete;
  UploadPipeline& operator=(const UploadPipeline&) = delete;

  // Injectable monotonic clock in milliseconds, the same shape Lifecycle uses.
  // Tests replace it so the deadline can be driven without real time.
  using ClockFn = osp::FixedFunction<std::uint64_t(), 16>;
  void SetClock(ClockFn clock) noexcept;

  // Override the directory-retention policy. Call before the first Run.
  void SetRetention(const sftp::RetentionPolicy& policy) noexcept { retention_ = policy; }

  // Run one upload-and-paste. `client` must be an INIT'ed SFTP channel and must
  // outlive the call. `now_unix` is the wall clock used for the remote filename
  // and the retention age rule; it is injected so tests are deterministic.
  //
  // Returns kBusy when a run is already in flight. Any failure before the
  // clipboard is published guarantees that neither set_text nor paste was
  // called, so the user sees an unchanged clipboard and no keystroke.
  Result<UploadReport> Run(sftp::Client& client, const ClipboardOps& clipboard, const InjectOps& inject,
                           std::uint64_t now_unix) noexcept;

  // Cheap, lock-free peek for the trigger path: lets the caller surface
  // "busy" without waiting. The authoritative check is the compare-and-swap
  // inside Run(); this is only an early-out for user feedback.
  bool in_flight() const noexcept { return in_flight_.load(std::memory_order_acquire); }

  // Bounded in-flight upload. A silent peer leaves the upload blocked in a read
  // with no EOF, so the single-flight flag would otherwise stay set forever and
  // the tool would look healthy while ignoring every later hotkey. The caller
  // must run this on a thread that is NOT the one blocked in Run -- on Windows
  // the main message loop arms a timer only while an upload is in flight, since
  // the worker's own loop is inside Run and cannot check anything. It compares
  // the injected clock against the in-flight upload's deadline and, on the first
  // call that finds it past, latches the expiry and counts it. The caller must
  // then tear the channel down exactly as it does for a dead pipe
  // (DropChannel + Lifecycle::Post(kChannelLost)); that teardown is what makes
  // the blocked read fail, so Run can unwind. False when nothing is in flight,
  // when upload_timeout_ms is 0 (deadline disabled), or before the deadline.
  bool UploadOverdue() noexcept;

  // Uploads aborted by upload_timeout_ms. Mirrors Lifecycle's failure counters.
  std::uint32_t UploadTimeoutCount() const noexcept { return upload_timeout_count_.load(std::memory_order_relaxed); }

 private:
  Status CaptureLocal(const ClipboardOps& clip, CapturedClip& out) noexcept;
  Status EnsureRemoteDir(sftp::Client& client, sftp::Path& out) noexcept;
  Status UploadAndVerify(sftp::Client& client, const CapturedClip& capture, const sftp::Path& dir,
                         sftp::Path& published, std::uint64_t now_unix) noexcept;
  Status Publish(const ClipboardOps& clip, const InjectOps& inject, const CapturedClip& capture,
                 const char* remote_path, bool& restored) noexcept;
  void Prune(sftp::Client& client, const sftp::Path& dir, std::uint64_t now_unix, UploadReport& report) noexcept;

  // True when the external deadline check has latched this flight. The
  // generation is compared so a latch from the previous upload cannot end a new
  // one; Run uses it to abort at a step boundary with kUploadTimeout.
  bool DeadlinePassed(std::uint32_t generation) const noexcept {
    return expired_generation_.load(std::memory_order_acquire) == generation;
  }
  std::uint64_t Now() const noexcept { return clock_(); }

  Config cfg_{};
  sftp::RetentionPolicy retention_{};
  std::uint32_t counter_ = 0u;
  // Cross-thread flag: the message loop reads it while the upload worker runs.
  // A non-lock-free atomic would hide a lock/heap dependency, so refuse it at
  // compile time (the same rule newosp applies to its own cross-thread flags).
  static_assert(std::atomic<bool>::is_always_lock_free, "the in-flight flag must be lock-free to stay allocation-free");
  std::atomic<bool> in_flight_{false};

  // Deadline bookkeeping, all touched by both the upload worker and whichever
  // thread runs the periodic check. A flight's generation keys its own start
  // time and expiry latch, so the previous upload's values are never attributed
  // to a new one. `active_generation_` is 0 when no upload is in flight.
  ClockFn clock_;
  std::atomic<std::uint32_t> flight_generation_{0};
  std::atomic<std::uint32_t> active_generation_{0};
  std::atomic<std::uint32_t> expired_generation_{0};
  std::atomic<std::uint64_t> flight_start_ms_{0};
  std::atomic<std::uint32_t> upload_timeout_count_{0};
};

}  // namespace picopaste

// picopaste -- Windows entry point.
//
// This binary is a GUI-subsystem app, so its CRT streams are not connected to
// anything: printf would write into the void and a failing run would look
// exactly like a passing one. The output below therefore goes through the
// process's standard handles with WriteFile, which does reach the terminal once
// a console is attached. Nothing here shows a MessageBox -- a modal dialog
// would hang a headless or CI invocation forever, and a process that cannot be
// observed is the failure mode this project exists to remove.
//
// Modes:
//   picopaste --selftest [--config PATH]
//       Run the per-capability table (design section 10) and exit 0 only when
//       no mandatory capability failed. This is the Windows verification
//       vehicle, so it is deliberately runnable from a terminal.
//   picopaste
//       Interactive tray mode. The main thread blocks on GetMessageW; a hotkey
//       posts WM_HOTKEY to this thread's queue and signals a worker thread that
//       runs the upload pipeline. Every periodic action waits on a kernel
//       object (the message loop, the worker's wait event, the child process
//       handle, or a WaitForMultipleObjects timeout), so idle CPU is zero.
//
// The platform seam for the pipeline lives here: Win32Platform below adapts the
// existing clipboard/inject modules into the function-pointer tables the
// platform-neutral core expects. It is the ONLY place those two layers meet.

#include "win32_util.hpp"

// windows.h pulls in shellapi.h only when WIN32_LEAN_AND_MEAN is undefined, and
// win32_util.hpp defines it, so CommandLineToArgvW needs this explicitly.
#include "clip.h"  // vendored dacap/clip: the same set_text path inject.cpp uses
#include "clipboard.hpp"
#include "hotkey.hpp"
#include "inject.hpp"
#include "selftest.hpp"
#include "single_instance.hpp"
#include "stream_win32.hpp"
#include "tray.hpp"

#include "../../core/app/lifecycle.hpp"        // picopaste::Lifecycle
#include "../../core/app/upload_pipeline.hpp"  // picopaste::UploadPipeline
#include "picopaste/config.hpp"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>

#include <atomic>
#include <new>
#include <shellapi.h>
#include <string>
#include <thread>
#include <type_traits>

namespace {

using picopaste::Config;
using picopaste::Error;
using picopaste::InjectOps;
using picopaste::Lifecycle;
using picopaste::LifecycleEvent;
using picopaste::LifecycleState;
using picopaste::Status;
using picopaste::TrayHealth;
using picopaste::UploadPipeline;
using picopaste::win32::SelfTestOptions;

// Exit codes. 0 is reserved for "everything mandatory passed".
constexpr std::int32_t kExitOk = 0;
constexpr std::int32_t kExitSelftestFailed = 1;

// One hotkey id; the WM_HOTKEY this produces is dispatched directly by the
// message loop (never by a window proc). The low-level hook re-posts the same
// message, so the loop needs no second trigger path.
constexpr std::int32_t kHotkeyId = 1;

// Thread-timer id for the in-flight deadline probe. SetTimer's hwnd == NULL
// form associates the timer with this thread and posts WM_TIMER with hwnd ==
// NULL, so it never touches the tray window another worker owns. The id is only
// a preference: the value SetTimer actually returns is what KillTimer takes.
constexpr UINT_PTR kUploadTimerId = 1;

// Tick span for that probe, derived from upload_timeout_ms.
//
// The worker is blocked inside a step while an upload is in flight, so it
// cannot run its own loop; this main-thread timer is the only thing that can
// notice a silent peer. It is armed on upload start and killed on upload end,
// which keeps the promise that idle CPU is exactly zero. A quarter of the
// deadline keeps detection proportional to it -- a 30 s deadline is probed
// every 1 s, never detected at 60 s -- while the bounds keep a sub-second
// deadline from being polled absurdly fast and a long one at most 1 Hz.
constexpr std::uint32_t kUploadTickMinMs = 250u;
constexpr std::uint32_t kUploadTickMaxMs = 1000u;
constexpr std::uint32_t UploadTickMs(std::uint32_t timeout_ms) noexcept {
  const std::uint32_t quarter = timeout_ms / 4u;
  if (quarter < kUploadTickMinMs) {
    return kUploadTickMinMs;
  }
  if (quarter > kUploadTickMaxMs) {
    return kUploadTickMaxMs;
  }
  return quarter;
}

// The hotkey path chosen for this run, so the tray tooltip can name it. Only
// the main thread writes and reads it.
picopaste::win32::HotkeyBackend g_hotkey_backend = picopaste::win32::HotkeyBackend::kNone;

// Longest config path accepted on the command line; a longer one is reported
// rather than cut short.
constexpr std::size_t kConfigPathChars = 512;

// One diagnostic line's ceiling. A truncated diagnostic is acceptable; a buffer
// overrun is not.
constexpr std::size_t kLineChars = 192;

// Posted by the worker when its lifecycle state (and therefore the tray colour)
// changes. The tray's Shell_NotifyIcon is called only from the main thread, the
// one that created the tray window; the worker never touches it directly.
constexpr UINT kWmWorkerStatus = WM_APP + 2;

enum class WorkerNotice : std::uint8_t {
  kHealth = 0,  // wParam is the TrayState to render
  kStopped = 1,
  kUploadStarted = 2,  // arms the main thread's deadline probe
  kUploadEnded = 3,    // disarms it, whatever the outcome
  kLocalFailure = 4,   // wParam is the Error code from a hotkey press that
                       // produced no upload; the main thread renders it as a
                       // one-shot balloon so a silent no-op looks like a dead
                       // hotkey and the user can act on it.
};

// RAII: attach to the console that launched us, and detach again on scope exit.
// FreeConsole is only called when we were the ones who attached, so a process
// that already owned a console is left as we found it.
class ParentConsole final {
 public:
  ParentConsole() noexcept : attached_(AttachConsole(ATTACH_PARENT_PROCESS) != 0) {}
  ~ParentConsole() noexcept {
    if (attached_ == true) {
      (void)FreeConsole();
    }
  }

  ParentConsole(const ParentConsole&) = delete;
  ParentConsole& operator=(const ParentConsole&) = delete;

 private:
  bool attached_ = false;
};

// RAII: the argument block CommandLineToArgvW allocates. Without this guard
// every early return would leak it.
class ArgvBlock final {
 public:
  ArgvBlock() noexcept : argv_(CommandLineToArgvW(GetCommandLineW(), &argc_)) {}
  ~ArgvBlock() noexcept {
    if (argv_ != nullptr) {
      (void)LocalFree(argv_);
    }
  }

  ArgvBlock(const ArgvBlock&) = delete;
  ArgvBlock& operator=(const ArgvBlock&) = delete;

  bool valid() const noexcept { return argv_ != nullptr; }
  std::int32_t count() const noexcept { return static_cast<std::int32_t>(argc_); }
  wchar_t* const* items() const noexcept { return argv_; }

 private:
  // Declaration order is load-bearing: argv_ is initialized by calling
  // CommandLineToArgvW(..., &argc_), and members initialize in declaration
  // order. With argc_ declared after argv_ it would be zero-initialized
  // *after* the call had written the real count, leaving a valid array with a
  // silently zero count -- which made every argument, and therefore --selftest
  // and --config, unreachable. Count first, then the array it describes.
  //
  // CommandLineToArgvW's signature takes `int*`; std::int32_t IS int on this
  // platform, so &argc_ is that int* by the platform contract.
  std::int32_t argc_ = 0;
  wchar_t** argv_ = nullptr;
};

// Write one already-formatted string to a standard handle. A handle that is
// absent (no console attached) is not an error: the exit code carries the
// result in that case.
void Emit(HANDLE handle, const char* text) noexcept {
  if ((nullptr == handle) || (INVALID_HANDLE_VALUE == handle)) {
    return;
  }
  const std::size_t length = std::strlen(text);
  if (0 == length) {
    return;
  }
  DWORD written = 0;
  (void)WriteFile(handle, text, static_cast<DWORD>(length), &written, nullptr);
}

// Same, with formatting. Used enough times here to be worth the indirection.
void EmitFormatted(HANDLE handle, const char* format, ...) noexcept {
  char line[kLineChars] = {};
  va_list args;
  va_start(args, format);
  const std::int32_t written = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written > 0) {
    Emit(handle, line);
  }
}

// Narrow a wide path into `out`. Returns false when it does not fit, so an
// over-long path is reported instead of being silently shortened.
bool NarrowPath(const wchar_t* wide, char* out, std::size_t out_chars) noexcept {
  const std::int32_t written =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<std::int32_t>(out_chars), nullptr, nullptr);
  return written > 0;
}

// Default config path used when --config is absent:
// <directory of the running executable>\picopaste.ini. A single-exe tool has
// exactly one natural place for its config, and the executable's own directory
// is stable without consulting the environment. The file need not exist:
// LoadConfig answers a missing file with the built-in defaults and reports
// created_defaults, which is the documented behaviour.
//
// Returns the number of characters written (excluding the terminator), or 0
// when the executable path cannot be read or the result does not fit in `out`.
std::size_t DerivedConfigPath(wchar_t* out, std::size_t out_chars) noexcept {
  if (out_chars < 2u) {
    return 0;
  }
  // Reserve one character for the terminator GetModuleFileNameW appends.
  const DWORD written = GetModuleFileNameW(nullptr, out, static_cast<DWORD>(out_chars - 1u));
  if ((written == 0u) || (static_cast<std::size_t>(written) >= (out_chars - 1u))) {
    return 0;  // unavailable, or truncated so the directory is not trustworthy
  }
  // Keep everything up to and including the last separator, dropping the
  // executable's file name so a sibling file can be appended.
  std::size_t dir_end = 0;
  for (std::size_t i = written; i > 0u; --i) {
    if ((out[i - 1] == L'\\') || (out[i - 1] == L'/')) {
      dir_end = i;
      break;
    }
  }
  if (dir_end == 0) {
    return 0;  // no directory component: cannot form a sibling path
  }
  const wchar_t kName[] = L"picopaste.ini";
  const std::size_t name_chars = sizeof(kName) / sizeof(kName[0]);  // includes the terminator
  if ((dir_end + name_chars) > out_chars) {
    return 0;  // would not fit
  }
  for (std::size_t i = 0; i < name_chars; ++i) {
    out[dir_end + i] = kName[i];
  }
  return (dir_end + name_chars) - 1u;
}

// Resolve the config path to hand to LoadConfig, as UTF-8 in `out`.
//
// With --config the caller's path is used verbatim; without it the derived
// default is used so LoadConfig still receives a real path and can answer a
// missing file with the built-in defaults. A path that does not fit is
// reported, never silently truncated.
bool ResolveConfigPath(HANDLE err, const wchar_t* config_path, char* out, std::size_t out_chars) noexcept {
  const bool from_user = (config_path != nullptr);
  wchar_t derived[kConfigPathChars] = {};
  const wchar_t* effective = config_path;
  if (from_user == false) {
    if (DerivedConfigPath(derived, sizeof(derived) / sizeof(derived[0])) == 0) {
      Emit(err, "FAIL config-path: could not derive the default config path\n");
      return false;
    }
    effective = derived;
  }
  if (NarrowPath(effective, out, out_chars) == false) {
    EmitFormatted(err, "FAIL config-path: the %s path exceeds %u bytes\n", (from_user ? "--config" : "default config"),
                  static_cast<unsigned>(out_chars));
    return false;
  }
  return true;
}

struct Options {
  bool selftest = false;
  const wchar_t* config_path = nullptr;  // borrowed from the argument block
};

Options ParseArgs(wchar_t* const* items, std::int32_t count) noexcept {
  Options options;
  for (std::int32_t i = 1; i < count; ++i) {
    if (0 == wcscmp(items[i], L"--selftest")) {
      options.selftest = true;
    } else if (0 == wcscmp(items[i], L"--config") && (i + 1) < count) {
      ++i;
      options.config_path = items[i];
    }
  }
  return options;
}

// ---------------------------------------------------------------------------
// Platform seam: the win32 clipboard and injector as the pipeline's two tables.
//
// Single-flight in the pipeline means at most one captured snapshot exists at a
// time, so this holds exactly one. `target_` is the foreground window captured
// when the hotkey fired, BEFORE anything touches the clipboard, and is read on
// the worker thread; it is atomic because the two threads cross here.
// ---------------------------------------------------------------------------
class Win32Platform final {
 public:
  Win32Platform() noexcept = default;
  Win32Platform(const Win32Platform&) = delete;
  Win32Platform& operator=(const Win32Platform&) = delete;

  void SetRestoreHint(bool restore) noexcept { restore_hint_ = restore; }
  void SetTarget(HWND window) noexcept { target_.store(window, std::memory_order_release); }

  picopaste::ClipboardOps Clipboard() noexcept {
    picopaste::ClipboardOps ops{};
    ops.ctx = this;
    ops.capture = &Win32Platform::Capture;
    ops.set_text = &Win32Platform::SetText;
    ops.restore = &Win32Platform::Restore;
    ops.release = &Win32Platform::Release;
    return ops;
  }

  InjectOps Inject() noexcept {
    InjectOps ops{};
    ops.ctx = this;
    ops.paste = &Win32Platform::Paste;
    return ops;
  }

 private:
  // CaptureClipboardImage opens, snapshots to one temp file, and closes the
  // clipboard before returning -- the contract the pipeline depends on.
  static Error Capture(void* ctx, const Config& cfg, picopaste::CapturedClip& out) noexcept {
    Win32Platform* self = static_cast<Win32Platform*>(ctx);
    const auto captured = picopaste::win32::CaptureClipboardImage(cfg);
    if (!captured.has_value()) {
      return captured.get_error();
    }
    self->snapshot_ = captured.value();
    out.local_path.assign(osp::TruncateToCapacity, self->snapshot_.path_utf8.c_str());
    out.bytes = self->snapshot_.bytes;
    out.restore_token = self;  // opaque: identifies the one snapshot
    out.captured = true;
    return Error::kOk;
  }

  static Error SetText(void* ctx, const char* utf8) noexcept {
    (void)ctx;
    if (nullptr == utf8) {
      return Error::kClipboardSetFailed;
    }
    const std::string text(utf8);  // bounded by the remote path cap
    if (clip::set_text(text) == false) {
      return Error::kClipboardSetFailed;
    }
    return Error::kOk;
  }

  static Error Restore(void* ctx, void* token) noexcept {
    (void)token;
    Win32Platform* self = static_cast<Win32Platform*>(ctx);
    if (L'\0' == self->snapshot_.wide_path[0]) {
      return Error::kClipboardSetFailed;
    }
    const Status s = picopaste::win32::SetClipboardPngFile(self->snapshot_.wide_path);
    return s.has_value() ? Error::kOk : s.get_error();
  }

  static void Release(void* ctx, void* token) noexcept {
    (void)token;
    Win32Platform* self = static_cast<Win32Platform*>(ctx);
    picopaste::win32::DeleteCapturedImage(self->snapshot_);
    self->snapshot_ = picopaste::win32::CapturedImage{};
  }

  // The focus guard from inject.cpp's DeliverPaste, split out so the pipeline
  // can put the text on the clipboard itself and observe the chord separately.
  static Error Paste(void* ctx, std::uint32_t delay_ms) noexcept {
    Win32Platform* self = static_cast<Win32Platform*>(ctx);
    const HWND target = self->target_.load(std::memory_order_acquire);
    if (nullptr == target) {
      // No baseline: never type into whatever happens to be focused.
      return Error::kFocusChanged;
    }
    if (delay_ms > 0) {
      Sleep(delay_ms);
    }
    if (GetForegroundWindow() != target) {
      return Error::kFocusChanged;
    }
    const picopaste::win32::SendChordResult chord = picopaste::win32::SendPasteChord();
    if (chord.ok == false) {
      return Error::kSendInputRejected;
    }
    // Let the terminal consume the paste before the image is restored over it;
    // DeliverPaste waited the same 150 ms. This is a bounded active wait on the
    // worker thread while a paste is in flight -- never an idle poll.
    if (self->restore_hint_) {
      Sleep(150);
    }
    return Error::kOk;
  }

  picopaste::win32::CapturedImage snapshot_{};
  std::atomic<HWND> target_{nullptr};
  bool restore_hint_ = false;
};

// Owner of the long-lived SFTP Client. Client is non-movable and ~128 KB, so it
// cannot live on the worker's stack (design section 5: a 256 KB stack with an
// 82 KB client leaves too little for the chunk buffers). Aligned storage plus
// placement new lets the worker rebuild it across a reconnect; Client is
// trivially destructible, which is what makes the placement new legal.
class ClientSlot final {
 public:
  ClientSlot() noexcept = default;
  ~ClientSlot() noexcept { Destroy(); }
  ClientSlot(const ClientSlot&) = delete;
  ClientSlot& operator=(const ClientSlot&) = delete;

  picopaste::sftp::Client* Get() noexcept { return client_; }

  void Create(picopaste::sftp::ByteStream stream) noexcept {
    Destroy();
    client_ = ::new (static_cast<void*>(storage_)) picopaste::sftp::Client(stream);
  }

  void Destroy() noexcept {
    if (nullptr != client_) {
      client_->~Client();
      client_ = nullptr;
    }
  }

 private:
  static_assert(std::is_trivially_destructible<picopaste::sftp::Client>::value,
                "ClientSlot placement-news Client and therefore needs a trivial destructor");
  alignas(picopaste::sftp::Client) std::byte storage_[sizeof(picopaste::sftp::Client)];
  picopaste::sftp::Client* client_ = nullptr;
};

// Map the lifecycle's traffic light onto the tray's own state enum.
picopaste::win32::TrayState MapTrayState(TrayHealth health) noexcept {
  switch (health) {
    case TrayHealth::kGreen:
      return picopaste::win32::TrayState::kHealthy;
    case TrayHealth::kRed:
      return picopaste::win32::TrayState::kError;
    case TrayHealth::kYellow:
    default:
      return picopaste::win32::TrayState::kWarning;
  }
}

// ---------------------------------------------------------------------------
// Upload worker. One thread, blocked on a waitable event when idle; it owns the
// ssh child and the SFTP client, runs the pipeline on each hotkey, and drives
// the Lifecycle machine for the whole process. Lifecycle is touched by this
// thread alone, so its state machine needs no locking.
// ---------------------------------------------------------------------------
class UploadWorker final {
 public:
  UploadWorker() noexcept = default;
  ~UploadWorker() noexcept { Stop(); }
  UploadWorker(const UploadWorker&) = delete;
  UploadWorker& operator=(const UploadWorker&) = delete;

  // Wire to the process-lifetime objects. Call once, on the main thread, before
  // Start().
  void Configure(const Config* cfg, HANDLE containment_job, UploadPipeline* pipeline, Lifecycle* lifecycle) noexcept {
    cfg_ = cfg;
    containment_job_ = containment_job;
    pipeline_ = pipeline;
    lifecycle_ = lifecycle;
    main_thread_ = GetCurrentThreadId();
    platform_.SetRestoreHint((nullptr != cfg) && cfg->restore_clipboard);
  }

  Status Start() noexcept {
    if (thread_.joinable()) {
      return Status::success();
    }
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    if (nullptr == wake_) {
      return Status::error(Error::kTempFileFailed);
    }
    try {
      thread_ = std::thread(&UploadWorker::Loop, this);
    } catch (...) {
      // Allocation failure under the job's commit ceiling must be reported,
      // never allowed to terminate the process.
      (void)CloseHandle(wake_);
      wake_ = nullptr;
      return Status::error(Error::kTempFileFailed);
    }
    return Status::success();
  }

  // Latch the stop and wake the loop without joining. The exit path calls this
  // BEFORE it terminates the ssh child, and the order is what makes the pair
  // safe. The worker publishes the child's handle immediately before it blocks
  // in a read; an abort that arrives before that store finds nothing to kill.
  // But it can only have arrived before the latch if the store came first, in
  // which case the abort does find the handle. One of the two always lands.
  void RequestStop() noexcept {
    quit_.store(true, std::memory_order_release);
    Wake();
  }

  // Idempotent. Signals the worker, joins it, then finishes the lifecycle.
  void Stop() noexcept {
    if (thread_.joinable()) {
      RequestStop();
      thread_.join();
    }
    DropChannel();
    if (nullptr != wake_) {
      (void)CloseHandle(wake_);
      wake_ = nullptr;
    }
    if (nullptr != lifecycle_) {
      lifecycle_->Stop();
      (void)lifecycle_->Post(LifecycleEvent::kStopped);
    }
    PostStopped();
  }

  void SetTarget(HWND window) noexcept { platform_.SetTarget(window); }
  void Wake() noexcept {
    if (nullptr != wake_) {
      (void)SetEvent(wake_);
    }
  }
  bool InFlight() const noexcept { return (nullptr != pipeline_) && pipeline_->in_flight(); }

  // Narrow thread-safe abort lever for the main thread's deadline probe. When
  // an upload is overdue the worker is blocked inside a step (a read on the ssh
  // pipe) and cannot run its own loop, so the main thread terminates the child
  // directly: the pipe breaks, the blocked read returns, and Run unwinds into
  // RunOnce's normal failure path. TerminateProcess only needs the process
  // handle we already own and does not touch ChildStream's own state -- the
  // worker stays the sole owner and closer of that object. The cached handle is
  // an atomic because ChildStream's member is not, and the worker clears it
  // before Close() so a late tick cannot target a retired channel.
  void AbortChannel() noexcept {
    const HANDLE process = abort_process_.load(std::memory_order_acquire);
    if (nullptr != process) {
      (void)TerminateProcess(process, 1);
    }
  }

 private:
  void Loop() noexcept {
    picopaste::win32::ComApartment com;
    (void)com.Init();  // WIC and the clipboard need COM on this thread
    lifecycle_->Start();
    (void)lifecycle_->Post(LifecycleEvent::kStart);  // Init -> Connecting
    PostHealth();
    // Establish the channel up front rather than waiting for the first hotkey:
    // a tray that sits on "connecting" until the user presses the chord looks
    // broken even though nothing is. A failure here posts ConnectFail, hands
    // the machine to Reconnecting and the supervisory retry below takes over;
    // a stop latched during the spawn is re-checked inside SpawnChannel.
    EnsureChannel();

    while (quit_.load(std::memory_order_acquire) == false) {
      const bool channel_alive = channel_.running();
      HANDLE handles[2] = {wake_, nullptr};
      DWORD count = 1;
      if (channel_alive) {
        handles[1] = channel_.process_handle();
        count = 2;
      }
      // When the link is down we wait with the backoff as a timeout, so the
      // reconnect is driven by the wait itself rather than by a polling tick.
      const DWORD timeout = NeedsRetry() ? lifecycle_->RetryDelayMs() : INFINITE;
      const DWORD woken = WaitForMultipleObjects(count, handles, FALSE, timeout);
      if (quit_.load(std::memory_order_acquire)) {
        break;
      }
      if (WAIT_OBJECT_0 == woken) {
        RunOnce();  // hotkey trigger
      } else if (channel_alive && ((WAIT_OBJECT_0 + 1) == woken)) {
        // The ssh child died while idle: a real channel-loss signal with no
        // polling. Degraded is entered from Ready; any other state is a no-op.
        DropChannel();
        (void)lifecycle_->Post(LifecycleEvent::kChannelLost);
        PostHealth();
      } else if (WAIT_TIMEOUT == woken) {
        // The supervisory timer fired: re-establish the channel.
        (void)lifecycle_->Post(LifecycleEvent::kRetry);
        EnsureChannel();
      } else {
        break;  // WAIT_FAILED or unexpected: stop rather than spin
      }
    }
  }

  // True when `error` says the SFTP channel, or the exchange already running on
  // it, is suspect; the only safe recovery is to drop the child so the
  // supervisory loop backs off and reconnects. The codes that return false are
  // produced entirely by the local clipboard and injector (or by the single-
  // flight guard); they say nothing about the link, so tearing it down would
  // report a failure that did not happen. Every other value falls through as
  // bad: an unrecognised code is far likelier to be a transport fault than a
  // local one, and hiding a real break is the worse error. kBusy never reaches
  // here; RunOnce returns for it before consulting this predicate.
  static bool ChannelIsBad(Error error) noexcept {
    switch (error) {
      // Clipboard capture (clipboard.cpp / UploadPipeline::CaptureLocal): local.
      case Error::kNoImageInClipboard:
      case Error::kClipboardOpenFailed:
      case Error::kClipboardLockFailed:
      case Error::kClipboardReadFailed:
      case Error::kImageTooLarge:
      case Error::kPngEncodeFailed:
      case Error::kTempFileFailed:
      // Paste injection (inject.cpp / Win32Platform::SetText/Paste): local.
      case Error::kClipboardSetFailed:
      case Error::kFocusChanged:
      case Error::kSendInputRejected:
        return false;
      default:
        // kChannelNotConnected / kChannelWriteFailed / kChannelReadFailed /
        // kSftpProtocolError / kRealpathFailed / kMkdirFailed / kOpenFailed /
        // kWriteFailed / kCloseFailed / kStatFailed / kRenameFailed /
        // kSizeMismatch / kBufferTooSmall -- all emitted by sftp::Client -- and
        // kUploadTimeout, whose ssh child the deadline probe has just killed.
        return true;
    }
  }

  void RunOnce() noexcept {
    if (EnsureChannel() == false) {
      return;  // EnsureChannel already posted the failure
    }
    const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
    // Tell the main thread an upload is about to start so it can arm the
    // deadline probe, and again however Run returns so it is always disarmed.
    PostUploadStarted();
    const auto result = pipeline_->Run(*client_.Get(), platform_.Clipboard(), platform_.Inject(), now);
    PostUploadEnded();
    if (result.has_value()) {
      PostHealth();
      return;
    }
    if (Error::kBusy == result.get_error()) {
      return;  // a queued trigger raced the running one; nothing happened
    }
    if (ChannelIsBad(result.get_error()) == false) {
      // A local, user-caused outcome: the link is healthy, so leave it up and
      // republish health rather than reporting a channel failure. But the hotkey
      // press produced no upload, and a silent no-op looks like a dead hotkey --
      // tell the main thread which capture step failed so it can surface that.
      PostLocalFailure(result.get_error());
      PostHealth();
      return;
    }
    // The channel or its exchange is suspect: drop it and let the supervisory
    // loop back off and retry, with the tray showing the failure.
    DropChannel();
    (void)lifecycle_->Post(LifecycleEvent::kConnectFail);
    PostHealth();
  }

  bool EnsureChannel() noexcept {
    if (nullptr != client_.Get()) {
      return true;
    }
    if (SpawnChannel() == false) {
      (void)lifecycle_->Post(LifecycleEvent::kConnectFail);
      PostHealth();
      return false;
    }
    (void)lifecycle_->Post(LifecycleEvent::kConnectOk);
    PostHealth();
    return true;
  }

  bool SpawnChannel() noexcept {
    wchar_t command[picopaste::win32::kMaxCommandLineChars] = {};
    if (BuildCommand(command, picopaste::win32::kMaxCommandLineChars) == false) {
      return false;
    }
    const picopaste::win32::ChildStreamOptions options{command, nullptr, containment_job_};
    const Status spawned = channel_.Spawn(options);
    if (spawned.has_value() == false) {
      return false;
    }
    abort_process_.store(channel_.process_handle(), std::memory_order_release);
    // A stop latched while this spawn was in flight could not have been seen by
    // the exit path's AbortChannel: that ran before the handle above existed, so
    // its load found nothing to terminate. Re-check the latch here instead of
    // blocking in Init, where Stop's join would have nothing to wake it.
    if (quit_.load(std::memory_order_acquire)) {
      DropChannel();
      return false;
    }
    client_.Create(channel_.stream());
    const Status inited = client_.Get()->Init();
    if (inited.has_value() == false) {
      DropChannel();
      return false;
    }
    return true;
  }

  void DropChannel() noexcept {
    // Clear the abort lever before closing: a tick that saw the live handle may
    // still be about to call TerminateProcess, which on a just-closed handle
    // fails harmlessly rather than reaching a recycled one.
    abort_process_.store(nullptr, std::memory_order_release);
    client_.Destroy();
    channel_.Close();
  }

  // `<ssh> -o ClearAllForwardings=yes -o BatchMode=yes -o LogLevel=ERROR -s <host> sftp`.
  // The options mirror the integration tests and keep a user's forwarding
  // config from firing on this channel.
  bool BuildCommand(wchar_t* out, std::size_t cap) const noexcept {
    wchar_t ssh[360] = {};
    wchar_t host[256] = {};
    if (picopaste::win32::Utf8ToWide(cfg_->ssh_command.c_str(), ssh, 360) == false) {
      return false;
    }
    if (picopaste::win32::Utf8ToWide(cfg_->host.c_str(), host, 256) == false) {
      return false;
    }
    const wchar_t* parts[] = {ssh, L" -o ClearAllForwardings=yes -o BatchMode=yes -o LogLevel=ERROR -s ", host,
                              L" sftp"};
    std::size_t used = 0u;
    for (const wchar_t* part : parts) {
      for (const wchar_t* p = part; L'\0' != *p; ++p) {
        if ((used + 1u) >= cap) {
          return false;  // report overflow rather than truncate the command
        }
        out[used] = *p;
        ++used;
      }
    }
    out[used] = L'\0';
    return true;
  }

  bool NeedsRetry() const noexcept {
    const LifecycleState state = lifecycle_->State();
    return (LifecycleState::kDegraded == state) || (LifecycleState::kReconnecting == state);
  }

  void PostUploadStarted() noexcept {
    (void)PostThreadMessageW(main_thread_, kWmWorkerStatus, static_cast<WPARAM>(0u),
                             static_cast<LPARAM>(WorkerNotice::kUploadStarted));
  }

  void PostUploadEnded() noexcept {
    (void)PostThreadMessageW(main_thread_, kWmWorkerStatus, static_cast<WPARAM>(0u),
                             static_cast<LPARAM>(WorkerNotice::kUploadEnded));
  }

  void PostHealth() noexcept {
    (void)PostThreadMessageW(main_thread_, kWmWorkerStatus, static_cast<WPARAM>(MapTrayState(lifecycle_->Health())),
                             static_cast<LPARAM>(WorkerNotice::kHealth));
  }

  void PostStopped() noexcept {
    (void)PostThreadMessageW(main_thread_, kWmWorkerStatus, static_cast<WPARAM>(picopaste::win32::TrayState::kError),
                             static_cast<LPARAM>(WorkerNotice::kStopped));
  }

  // A capture or paste step that returned a local error -- the channel stays
  // healthy, but the hotkey press did nothing visible. The main thread maps the
  // error to a one-shot tray balloon; the link's health is unchanged.
  void PostLocalFailure(Error err) noexcept {
    (void)PostThreadMessageW(main_thread_, kWmWorkerStatus, static_cast<WPARAM>(err),
                             static_cast<LPARAM>(WorkerNotice::kLocalFailure));
  }

  Win32Platform platform_{};
  picopaste::win32::ChildStream channel_{};
  ClientSlot client_{};
  const Config* cfg_ = nullptr;
  HANDLE containment_job_ = nullptr;
  HANDLE wake_ = nullptr;
  UploadPipeline* pipeline_ = nullptr;
  Lifecycle* lifecycle_ = nullptr;
  // The ssh child's process handle, published for AbortChannel. Set on spawn,
  // cleared before the channel closes; read by the main thread only.
  std::atomic<HANDLE> abort_process_{nullptr};
  DWORD main_thread_ = 0;
  std::atomic<bool> quit_{false};
  std::thread thread_{};
};

// ---------------------------------------------------------------------------
// Interactive mode
// ---------------------------------------------------------------------------

bool LoadInteractiveConfig(HANDLE out, HANDLE err, const wchar_t* config_path, Config& out_cfg) noexcept {
  char narrow[kConfigPathChars] = {};
  if (ResolveConfigPath(err, config_path, narrow, sizeof(narrow)) == false) {
    return false;
  }

  bool created_defaults = false;
  const auto loaded = picopaste::LoadConfig(narrow, &created_defaults);
  if (loaded.has_value() == false) {
    EmitFormatted(err, "FAIL config-load: error %u\n", static_cast<unsigned>(loaded.get_error()));
    return false;
  }
  if (created_defaults == true) {
    Emit(out, "note: no config file found; using defaults\n");
  }
  out_cfg = loaded.value();
  return true;
}

// Build the hover text, appending which hotkey path is live. A hook is a
// different mechanism with a different failure mode, so it must not be
// indistinguishable from a normal registration in the UI.
const wchar_t* TipFor(picopaste::win32::TrayState state, bool stopped) noexcept {
  if (stopped) {
    return L"picopaste: stopped";
  }
  const wchar_t* base = nullptr;
  switch (state) {
    case picopaste::win32::TrayState::kHealthy:
      base = L"picopaste: ready";
      break;
    case picopaste::win32::TrayState::kError:
      base = L"picopaste: channel lost, retrying";
      break;
    case picopaste::win32::TrayState::kWarning:
    default:
      base = L"picopaste: connecting";
      break;
  }
  static wchar_t tip[160] = {};
  const wchar_t* path = (g_hotkey_backend == picopaste::win32::HotkeyBackend::kHook)
                            ? L"hotkey: low-level hook (chord was taken)"
                            : L"hotkey: registered";
  (void)std::swprintf(tip, sizeof(tip) / sizeof(tip[0]), L"%s (%s)", base, path);
  return tip;
}

// Balloon text for a transition into a non-healthy state. The tray only changes
// colour, which is easy to miss on a busy taskbar; the design's rule is that a
// failure must be noticed, so the transition is announced once.
const wchar_t* NoticeFor(picopaste::win32::TrayState state) noexcept {
  switch (state) {
    case picopaste::win32::TrayState::kError:
      return L"channel lost - retrying in the background";
    case picopaste::win32::TrayState::kWarning:
    default:
      return L"reconnecting";
  }
}

// Maps a local, user-caused pipeline error to a one-line balloon the user can
// act on. Anything not in this list is intentionally silent: a hotkey that
// hits nothing for some unforeseen reason should not invent a story.
const wchar_t* NoticeForLocalError(Error err) noexcept {
  switch (err) {
    case Error::kNoImageInClipboard:
      return L"clipboard has no image";
    case Error::kImageTooLarge:
      return L"image too large (max 20 MB)";
    case Error::kPngEncodeFailed:
      return L"image encode failed";
    case Error::kClipboardOpenFailed:
      return L"another program is holding the clipboard";
    case Error::kClipboardLockFailed:
      return L"clipboard lock failed";
    case Error::kClipboardReadFailed:
      return L"could not read the image from the clipboard";
    case Error::kTempFileFailed:
      return L"could not create temp file";
    default:
      return nullptr;
  }
}

std::int32_t RunInteractive(HINSTANCE instance, HANDLE out, HANDLE err, const wchar_t* config_path) noexcept {
  Config config = picopaste::DefaultConfig();
  if (LoadInteractiveConfig(out, err, config_path, config) == false) {
    return kExitSelftestFailed;
  }
  if (config.host.empty()) {
    Emit(err, "picopaste: config 'host' is empty; set host = <ssh alias> and retry.\n");
    return kExitSelftestFailed;
  }

  // Function-locals with static storage: the SFTP Client is ~128 KB and must
  // never sit on a 256 KB worker stack (design section 5). Process-lifetime
  // storage is also exactly the lifetime the design requires for the client.
  static UploadPipeline pipeline(config);
  static Lifecycle lifecycle;
  static UploadWorker worker;

  picopaste::win32::SingleInstance single;
  picopaste::win32::Tray tray;

  std::uint32_t owner_pid = 0;
  wchar_t owner_image[picopaste::win32::kOwnerImageChars] = {};
  Status setup = single.Acquire(&owner_pid, owner_image, picopaste::win32::kOwnerImageChars);
  if (!setup) {
    if (Error::kSingleInstanceExists == setup.get_error()) {
      char owner_narrow[picopaste::win32::kOwnerImageChars * 3] = {};
      (void)picopaste::win32::WideToUtf8(owner_image, owner_narrow, sizeof(owner_narrow));
      EmitFormatted(err, "picopaste: already running (pid %u, image %s); refusing a second instance.\n",
                    static_cast<unsigned>(owner_pid), owner_narrow);
    } else {
      EmitFormatted(err, "picopaste: could not create the single-instance mutex (error %u)\n",
                    static_cast<unsigned>(setup.get_error()));
    }
  }
  if (setup) {
    setup = single.SetupJobObjects(config.job_memory_limit_mb);
  }
  if (setup) {
    setup = tray.Create(instance, L"picopaste: starting");
  }
  if (setup) {
    setup = tray.Show();
  }

  const auto binding = picopaste::win32::ParseHotkey(config.hotkey.c_str());
  if (setup && !binding) {
    setup = Status::error(binding.get_error());
  }
  // RAII: whichever path installs is released on every return below, including
  // the early worker-start failure. The class also unhooks before a Restart
  // relaunch (see the message loop).
  picopaste::win32::HotkeyRegistration hotkey;
  DWORD hotkey_error = 0;
  if (setup) {
    const Status installed = hotkey.Install(nullptr, kHotkeyId, binding.value());
    hotkey_error = hotkey.last_error();
    if (!installed) {
      setup = installed;
    }
  }
  if (!setup) {
    EmitFormatted(err, "picopaste: startup failed (error %u, hotkey %lu)\n", static_cast<unsigned>(setup.get_error()),
                  static_cast<unsigned long>(hotkey_error));
    return kExitSelftestFailed;
  }
  g_hotkey_backend = hotkey.backend();
  // Surface which path is live at startup as well as in the tooltip.
  EmitFormatted(out, "picopaste: hotkey %s via %s\n", binding.value().display.c_str(),
                picopaste::win32::HotkeyBackendName(hotkey.backend()));

  worker.Configure(&config, single.containment_job(), &pipeline, &lifecycle);
  const Status started = worker.Start();
  if (!started) {
    EmitFormatted(err, "picopaste: could not start the upload worker (error %u)\n",
                  static_cast<unsigned>(started.get_error()));
    hotkey.Uninstall();
    single.Close();
    tray.Destroy();
    return kExitSelftestFailed;
  }

  // Prime this thread's message queue so the worker's first PostThreadMessageW
  // has a queue to land in even if it posts before the loop's first GetMessageW.
  MSG prime{};
  (void)PeekMessageW(&prime, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

  // The in-flight deadline probe. Zero means "not armed"; the value is the id
  // SetTimer returned, which KillTimer needs because the hwnd == NULL form may
  // replace the requested id with one of its own.
  UINT_PTR upload_timer = 0;
  // Last health the tray was told about, so a balloon fires on the transition
  // into a bad state rather than on every health post the worker sends.
  auto last_state = picopaste::win32::TrayState::kHealthy;
  const auto disarm_timer = [&upload_timer]() noexcept {
    if (0u != upload_timer) {
      (void)KillTimer(nullptr, upload_timer);
      upload_timer = 0u;
    }
  };

  std::int32_t exit_code = kExitOk;
  bool loop_error = false;
  // Set when this loop released the chord for a tray Restart; see the re-install
  // below the dispatch.
  bool chord_released = false;
  for (;;) {
    MSG msg{};
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (0 == got) {  // WM_QUIT
      exit_code = static_cast<std::int32_t>(msg.wParam);
      break;
    }
    if (-1 == got) {
      loop_error = true;
      break;
    }
    if ((picopaste::win32::kWmHotkey == msg.message) && (nullptr == msg.hwnd)) {
      // WM_HOTKEY registered against the thread (hwnd == nullptr) arrives here,
      // not at the tray window. The message loop itself never blocks on I/O.
      if (worker.InFlight()) {
        if (config.notify_enabled) {
          tray.Notify(L"picopaste", L"an upload is already in flight", false);
        }
      } else {
        worker.SetTarget(picopaste::win32::CaptureForegroundWindow());
        worker.Wake();
      }
      continue;
    }
    if (kWmWorkerStatus == msg.message) {
      const auto notice = static_cast<WorkerNotice>(msg.lParam);
      if (WorkerNotice::kUploadStarted == notice) {
        // Arm the probe only while an upload is in flight. A disabled deadline
        // (0) never latches, so arming then would be pure wake-ups; the idle
        // promise is exactly zero CPU, not "near zero".
        if ((0u != config.upload_timeout_ms) && (0u == upload_timer)) {
          const std::uint32_t tick_ms = UploadTickMs(config.upload_timeout_ms);
          upload_timer = SetTimer(nullptr, kUploadTimerId, static_cast<UINT>(tick_ms), nullptr);
          if (0u == upload_timer) {
            // A probe that will not arm is the one failure this loop must not
            // swallow: without it a hung upload can never be torn down and the
            // configured deadline becomes inert. Capture the error before any
            // other call can clobber it, say so where a console can see it, and
            // turn the tray red. The worker owns the lifecycle machine on its
            // own thread, so the main thread renders this state directly rather
            // than posting an event into it.
            const DWORD timer_error = GetLastError();
            EmitFormatted(err, "picopaste: could not arm the upload deadline probe (error %lu)\n",
                          static_cast<unsigned long>(timer_error));
            tray.SetState(picopaste::win32::TrayState::kError, L"picopaste: upload deadline unavailable");
            if (config.notify_enabled) {
              tray.Notify(L"picopaste", L"upload deadline unavailable - a hung upload may not be torn down", true);
            }
            last_state = picopaste::win32::TrayState::kError;
          }
        }
      } else if (WorkerNotice::kUploadEnded == notice) {
        disarm_timer();
      } else if (WorkerNotice::kLocalFailure == notice) {
        // One-shot balloon: a hotkey press that produced no upload would
        // otherwise look exactly like a dead hotkey, with no state change to
        // announce it. We do not touch the tray state or the tooltip -- the
        // link is healthy -- we only say what went wrong.
        const wchar_t* detail = NoticeForLocalError(static_cast<Error>(msg.wParam));
        if (detail != nullptr && config.notify_enabled) {
          tray.Notify(L"picopaste", detail, true);
        }
      } else if (WorkerNotice::kStopped == notice) {
        disarm_timer();
        const auto state = static_cast<picopaste::win32::TrayState>(msg.wParam);
        tray.SetState(state, TipFor(state, true));
      } else {
        const auto state = static_cast<picopaste::win32::TrayState>(msg.wParam);
        // Announce the transition into a bad state once. Every health post also
        // sets the tooltip, so repeating the balloon would turn a failure into
        // noise; notify_enabled switches it off entirely.
        if (config.notify_enabled && (state != last_state) && (picopaste::win32::TrayState::kHealthy != state)) {
          tray.Notify(L"picopaste", NoticeFor(state), true);
        }
        last_state = state;
        tray.SetState(state, TipFor(state, false));
      }
      continue;
    }
    if ((WM_TIMER == msg.message) && (nullptr == msg.hwnd) && (0u != upload_timer) &&
        (static_cast<UINT_PTR>(msg.wParam) == upload_timer)) {
      // The worker is blocked inside a step and cannot run its own loop, so this
      // is the only thread that can notice a silent peer. Latching the expiry is
      // one atomic load plus one monotonic clock read; terminating the ssh child
      // breaks its pipe, so the worker's blocked read returns and Run unwinds
      // into RunOnce's normal failure path (drop, degrade, red tray). The timer
      // stays armed until kUploadEnded so a missed first abort is retried.
      if (pipeline.UploadOverdue()) {
        worker.AbortChannel();
      }
      continue;
    }
    if ((msg.hwnd == tray.hwnd()) && (WM_COMMAND == msg.message)) {
      // The tray's Restart item relaunches this process before the loop unwinds.
      // Release the chord first so the replacement can claim it the moment it
      // starts; on the hook path this also stops the old callback from
      // swallowing the replacement's keystrokes.
      hotkey.Uninstall();
      chord_released = true;
    }
    (void)TranslateMessage(&msg);
    (void)DispatchMessageW(&msg);
    if (chord_released) {
      // A Restart that failed leaves this process running -- tray.cpp reports it
      // and does not quit -- so the chord released above has to come back, or
      // the hotkey is dead for the rest of this process's life while the app
      // still looks alive. A pending WM_QUIT is how the tray says it did decide
      // to leave (Quit, a successful Restart, or WM_CLOSE), and then the chord
      // stays released for the replacement.
      MSG pending{};
      const bool leaving = (PeekMessageW(&pending, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE) != 0);
      chord_released = false;
      if (!leaving) {
        // One attempt, not a retry loop: nobody else can have taken the chord
        // yet, so a failure here means the registration is gone for good and
        // repeating it on every queued message would only spin.
        const Status reinstalled = hotkey.Install(nullptr, kHotkeyId, binding.value());
        if (!reinstalled) {
          // Say so. A hotkey that is silently gone is the very failure this
          // branch exists to prevent, so it must not end in a discarded result.
          tray.SetState(picopaste::win32::TrayState::kError, L"picopaste: hotkey lost after a failed restart");
          if (config.notify_enabled) {
            tray.Notify(L"picopaste", L"hotkey could not be re-registered", true);
          }
        }
      }
    }
  }

  // Leaving the loop: the worker may be blocked on a live ssh child, in which
  // case Stop's join would wait forever. Latch the stop first and only then
  // terminate the child -- see RequestStop for why that order is the one that
  // leaves no window, and SpawnChannel for the matching re-check. The worker
  // also blocks in the SFTP handshake: SpawnChannel calls Client::Init, a
  // synchronous read, before RunOnce reaches the pipeline, so during that window
  // in_flight() is still false and an InFlight() guard would skip the one case
  // where the join has nothing to wake. Disarm the probe last so no wake-up
  // outlives this queue.
  worker.RequestStop();
  worker.AbortChannel();
  disarm_timer();
  worker.Stop();
  hotkey.Uninstall();
  tray.Destroy();
  single.Close();
  return loop_error ? kExitSelftestFailed : exit_code;
}

std::int32_t RunSelftest(HANDLE out, HANDLE err, const wchar_t* config_path) noexcept {
  char narrow[kConfigPathChars] = {};
  if (ResolveConfigPath(err, config_path, narrow, sizeof(narrow)) == false) {
    return kExitSelftestFailed;
  }

  bool created_defaults = false;
  const auto loaded = picopaste::LoadConfig(narrow, &created_defaults);
  if (loaded.has_value() == false) {
    // The error type has no stringifier yet, so report the numeric code rather
    // than a vague "failed".
    EmitFormatted(err, "FAIL config-load: error %u\n", static_cast<unsigned>(loaded.get_error()));
    return kExitSelftestFailed;
  }
  if (created_defaults == true) {
    Emit(out, "note: no config file found; using defaults\n");
  }

  SelfTestOptions options;
  options.config = &loaded.value();
  options.e2e = true;

  const Status result = picopaste::win32::RunSelfTest(options);
  if (result.has_value() == false) {
    EmitFormatted(out, "selftest: FAILED (error %u)\n", static_cast<unsigned>(result.get_error()));
    return kExitSelftestFailed;
  }

  Emit(out, "selftest: all mandatory capabilities passed\n");
  return kExitOk;
}

}  // namespace

// The exception specification on this definition must match the one Win32
// declares for wWinMain, which is not noexcept, so this entry point is the one
// function here that cannot carry it.
//
// The bare `int` return and `int show` parameter are the CRT's fixed contract
// for wWinMain, so they stay as the platform declares them even though the rest
// of this file uses <cstdint> types.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
  (void)previous;
  (void)command_line;
  (void)show;

  const ParentConsole console;
  const ArgvBlock arguments;

  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);

  if (arguments.valid() == false) {
    Emit(err, "FAIL argv: could not read the process command line\n");
    return kExitSelftestFailed;
  }

  const Options options = ParseArgs(arguments.items(), arguments.count());
  if (options.selftest == true) {
    return RunSelftest(out, err, options.config_path);
  }
  return RunInteractive(instance, out, err, options.config_path);
}

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
//       Interactive tray mode. NOT WIRED YET: the hotkey/tray/paste loop is the
//       remaining integration work. This path says so and exits non-zero rather
//       than starting a daemon that silently does nothing -- reporting success
//       for work that did not happen is the other failure mode this project
//       exists to remove.

#include "win32_util.hpp"

// windows.h pulls in shellapi.h only when WIN32_LEAN_AND_MEAN is undefined, and
// win32_util.hpp defines it, so CommandLineToArgvW needs this explicitly.
#include <shellapi.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "picopaste/config.hpp"
#include "selftest.hpp"

namespace {

using picopaste::LoadConfig;
using picopaste::Status;
using picopaste::win32::SelfTestOptions;

// Exit codes. 0 is reserved for "everything mandatory passed".
constexpr std::int32_t kExitOk = 0;
constexpr std::int32_t kExitSelftestFailed = 1;
constexpr std::int32_t kExitNotImplemented = 2;

// Longest config path accepted on the command line; a longer one is reported
// rather than cut short.
constexpr std::size_t kConfigPathChars = 512;

// One diagnostic line's ceiling. A truncated diagnostic is acceptable; a buffer
// overrun is not.
constexpr std::size_t kLineChars = 192;

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
  // CommandLineToArgvW's signature takes `int*`, so this member is an int by
  // the platform contract and is narrowed back on the way out.
  wchar_t** argv_ = nullptr;
  int argc_ = 0;
};

// Write one already-formatted string to a standard handle. A handle that is
// absent (no console attached) is not an error: the exit code carries the
// result in that case.
void Emit(HANDLE handle, const char* text) noexcept {
  if (nullptr == handle || INVALID_HANDLE_VALUE == handle) {
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
  const int written = std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (written > 0) {
    Emit(handle, line);
  }
}

// Narrow a wide argument into `out`. Returns false when it does not fit, so an
// over-long path is reported instead of being silently shortened.
bool NarrowPath(const wchar_t* wide, char* out, std::size_t out_chars) noexcept {
  const int written =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(out_chars), nullptr, nullptr);
  return written > 0;
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

std::int32_t RunSelftest(HANDLE out, HANDLE err, const wchar_t* config_path) noexcept {
  char narrow[kConfigPathChars] = {};
  const char* path = nullptr;
  if (config_path != nullptr) {
    if (NarrowPath(config_path, narrow, sizeof(narrow)) == false) {
      EmitFormatted(err, "FAIL config-path: the --config path exceeds %u bytes\n",
                    static_cast<unsigned>(kConfigPathChars));
      return kExitSelftestFailed;
    }
    path = narrow;
  }

  bool created_defaults = false;
  const auto loaded = LoadConfig(path, &created_defaults);
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

std::int32_t RunInteractive(HANDLE err) noexcept {
  Emit(err,
       "picopaste: interactive mode is not wired yet -- the tray/hotkey/paste loop is still\n"
       "           missing, so there is nothing to run. This build can verify itself only:\n"
       "           run `picopaste --selftest` from a terminal.\n");
  return kExitNotImplemented;
}

}  // namespace

// The exception specification on this definition must match the one Win32
// declares for wWinMain, which is not noexcept, so this entry point is the one
// function here that cannot carry it.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
  (void)instance;
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
  return RunInteractive(err);
}

// picopaste -- per-capability self-check implementation.
#include "selftest.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "picopaste/sftp/client.hpp"
#include "clipboard.hpp"
#include "hotkey.hpp"
#include "single_instance.hpp"
#include "stream_win32.hpp"
#include "win32_util.hpp"

namespace picopaste::win32 {
namespace {

int g_passed = 0;
int g_failed = 0;

void PrintRaw(const char* text) noexcept {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out == nullptr || out == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  (void)WriteFile(out, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

void PrintLine(const char* text) noexcept {
  PrintRaw(text);
  PrintRaw("\r\n");
}

void Report(const char* capability, bool pass, const char* detail) noexcept {
  char line[512] = {};
  (void)std::snprintf(line, sizeof(line), "[%s] %-22s %s", pass ? "PASS" : "FAIL", capability,
                      detail != nullptr ? detail : "");
  PrintLine(line);
  if (pass) {
    ++g_passed;
  } else {
    ++g_failed;
  }
}

void ReportSkip(const char* capability, const char* detail) noexcept {
  char line[512] = {};
  (void)std::snprintf(line, sizeof(line), "[SKIP] %-22s %s", capability, detail != nullptr ? detail : "");
  PrintLine(line);
}

bool QueryMemory(std::uint64_t* working_set, std::uint64_t* private_bytes) noexcept {
  using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS_EX*, DWORD);
  HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
  if (kernel == nullptr) {
    return false;
  }
  GetProcessMemoryInfoFn query = reinterpret_cast<GetProcessMemoryInfoFn>(
      GetProcAddress(kernel, "K32GetProcessMemoryInfo"));
  if (query == nullptr) {
    return false;
  }
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (query(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return false;
  }
  if (working_set != nullptr) {
    *working_set = counters.WorkingSetSize;
  }
  if (private_bytes != nullptr) {
    *private_bytes = counters.PrivateUsage;
  }
  return true;
}

// --- 1. clipboard formats ---------------------------------------------------
void CheckClipboard() noexcept {
  const ClipboardFormats formats = InspectClipboardFormats();
  char detail[256] = {};
  (void)std::snprintf(detail, sizeof(detail), "PNG=%d DIBV5=%d DIB=%d any=%d",
                      formats.has_png ? 1 : 0, formats.has_dibv5 ? 1 : 0, formats.has_dib ? 1 : 0,
                      formats.has_any_image ? 1 : 0);
  Report("clipboard-formats", true, detail);
}

// --- 2. WIC PNG encode from a synthetic DIB ---------------------------------
void CheckWicEncode() noexcept {
  // 4x4 32bpp BI_RGB DIB (BITMAPINFOHEADER + pixels), bottom-up like a real
  // clipboard DIB so the flipping path is exercised.
  constexpr std::uint32_t kWidth = 4;
  constexpr std::uint32_t kHeight = 4;
  std::uint8_t dib[40 + kWidth * kHeight * 4] = {};
  std::int32_t width = static_cast<std::int32_t>(kWidth);
  std::int32_t height = static_cast<std::int32_t>(kHeight);
  std::uint16_t planes = 1;
  std::uint16_t bit_count = 32;
  std::int32_t header_size = 40;
  std::memcpy(dib + 0, &header_size, 4);
  std::memcpy(dib + 4, &width, 4);
  std::memcpy(dib + 8, &height, 4);
  std::memcpy(dib + 12, &planes, 2);
  std::memcpy(dib + 14, &bit_count, 2);
  for (std::uint32_t i = 0; i < kWidth * kHeight; ++i) {
    dib[40 + i * 4 + 0] = 0x20;
    dib[40 + i * 4 + 1] = 0x80;
    dib[40 + i * 4 + 2] = static_cast<std::uint8_t>(0x40 + i * 4);
    dib[40 + i * 4 + 3] = 0x00;  // reserved, must still encode opaque
  }

  wchar_t temp_dir[MAX_PATH + 1] = {};
  if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
    Report("wic-encode", false, "GetTempPathW failed");
    return;
  }
  wchar_t path[MAX_PATH + 1] = {};
  if (GetTempFileNameW(temp_dir, L"cct", 0, path) == 0) {
    Report("wic-encode", false, "GetTempFileNameW failed");
    return;
  }

  const Result<CapturedImage> encoded =
      EncodeDibToPngFile(dib, sizeof(dib), path, 4u * 1024u * 1024u);
  bool ok = false;
  char detail[256] = {};
  if (encoded.has_value()) {
    // Verify the file really starts with the PNG signature.
    UniqueHandle file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    std::uint8_t signature[8] = {};
    DWORD got = 0;
    const bool read = file.valid() &&
                      ReadFile(file.get(), signature, 8, &got, nullptr) != 0 && got == 8;
    const std::uint8_t expected[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    ok = read && std::memcmp(signature, expected, 8) == 0;
    (void)std::snprintf(detail, sizeof(detail), "png=%llu bytes sig=%s",
                        static_cast<unsigned long long>(encoded.value().bytes), ok ? "ok" : "bad");
  } else {
    (void)std::snprintf(detail, sizeof(detail), "encode failed (error=%u)",
                        static_cast<unsigned>(encoded.get_error()));
  }
  DeleteFileW(path);
  Report("wic-encode", ok, detail);
}

// --- 3. SendInput -----------------------------------------------------------
void CheckSendInput() noexcept {
  // A no-op key (F24) rather than the paste chord: this only proves the input
  // stream accepts our events from this integrity level.
  INPUT events[2] = {};
  for (int i = 0; i < 2; ++i) {
    events[i].type = INPUT_KEYBOARD;
    events[i].ki.wVk = 0x87;  // VK_F24
    events[i].ki.dwFlags = (i == 1) ? KEYEVENTF_KEYUP : 0;
  }
  const UINT inserted = SendInput(2, events, sizeof(INPUT));
  char detail[256] = {};
  (void)std::snprintf(detail, sizeof(detail), "inserted %u of 2 (uiAccess/elevation check)",
                      static_cast<unsigned>(inserted));
  Report("sendinput", inserted == 2, detail);
}

// --- 4. RegisterHotKey ------------------------------------------------------
void CheckHotkey(const Config* config) noexcept {
  const char* text = (config != nullptr && config->hotkey.empty() == false) ? config->hotkey.c_str()
                                                                            : "alt+shift+v";
  const Result<HotkeyBinding> parsed = ParseHotkey(text);
  if (parsed.has_value() == false) {
    char detail[256] = {};
    (void)std::snprintf(detail, sizeof(detail), "parse failed for \"%s\"", text);
    Report("register-hotkey", false, detail);
    return;
  }
  DWORD last_error = 0;
  const Status registered = RegisterHotkey(nullptr, 0xCC01, parsed.value(), &last_error);
  char detail[256] = {};
  if (registered.has_value()) {
    UnregisterHotkey(nullptr, 0xCC01);
    (void)std::snprintf(detail, sizeof(detail), "%s registered and released",
                        parsed.value().display.c_str());
    Report("register-hotkey", true, detail);
  } else {
    (void)std::snprintf(detail, sizeof(detail), "%s refused (GetLastError=%lu)",
                        parsed.value().display.c_str(), static_cast<unsigned long>(last_error));
    Report("register-hotkey", false, detail);
  }
}

// --- 5. single-instance mutex ----------------------------------------------
void CheckSingleInstance() noexcept {
  UniqueHandle mutex(CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName));
  char detail[256] = {};
  if (mutex.valid() == false) {
    (void)std::snprintf(detail, sizeof(detail), "CreateMutexW failed (GetLastError=%lu)",
                        static_cast<unsigned long>(GetLastError()));
    Report("single-instance", false, detail);
    return;
  }
  const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);
  (void)std::snprintf(detail, sizeof(detail), "mutex %s", existed ? "already held by another instance" : "acquired (no other instance)");
  Report("single-instance", true, detail);
}

// --- 6. Job Object limits ---------------------------------------------------
void CheckJobObjects(const Config* config) noexcept {
  const std::uint32_t limit_mb = (config != nullptr) ? config->job_memory_limit_mb : 32u;

  // Memory job: assign this process and read the enforced number back. No
  // KILL_ON_JOB_CLOSE here, so closing the handle at the end cannot kill us.
  HANDLE memory_job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory_info{};
  memory_info.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
  memory_info.JobMemoryLimit = static_cast<SIZE_T>(limit_mb) * 1024u * 1024u;
  bool ok = false;
  std::uint64_t enforced_mb = 0;
  if (memory_job != nullptr &&
      SetInformationJobObject(memory_job, JobObjectExtendedLimitInformation, &memory_info,
                              sizeof(memory_info)) != 0 &&
      AssignProcessToJobObject(memory_job, GetCurrentProcess()) != 0) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION queried{};
    if (QueryInformationJobObject(memory_job, JobObjectExtendedLimitInformation, &queried,
                                  sizeof(queried), nullptr) != 0) {
      enforced_mb = queried.JobMemoryLimit / (1024u * 1024u);
      ok = (enforced_mb == limit_mb);
    }
  }
  if (memory_job != nullptr) {
    CloseHandle(memory_job);
  }

  // Containment job: verify the flag can be set, but do not assign this process
  // (closing a KILL_ON_JOB_CLOSE job we belong to would terminate the test).
  HANDLE containment = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION containment_info{};
  containment_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  bool kill_ok = false;
  if (containment != nullptr &&
      SetInformationJobObject(containment, JobObjectExtendedLimitInformation, &containment_info,
                              sizeof(containment_info)) != 0) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION queried{};
    kill_ok = QueryInformationJobObject(containment, JobObjectExtendedLimitInformation, &queried,
                                        sizeof(queried), nullptr) != 0 &&
              (queried.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
  }
  if (containment != nullptr) {
    CloseHandle(containment);
  }

  char detail[256] = {};
  (void)std::snprintf(detail, sizeof(detail), "job-memory=%llu MB kill-on-close=%s",
                      static_cast<unsigned long long>(enforced_mb), kill_ok ? "settable" : "FAILED");
  Report("job-object", ok && kill_ok, detail);
}

// --- 7. child spawn + SFTP reachability -------------------------------------
void CheckSftp(const Config* config) noexcept {
  if (config == nullptr || config->host.empty()) {
    ReportSkip("sftp-reachability", "no host configured");
    return;
  }
  const char* ssh = config->ssh_command.empty() ? "ssh" : config->ssh_command.c_str();
  wchar_t ssh_wide[128] = {};
  wchar_t host_wide[256] = {};
  if (Utf8ToWide(ssh, ssh_wide, 128) == false || Utf8ToWide(config->host.c_str(), host_wide, 256) == false) {
    Report("sftp-reachability", false, "command line conversion failed");
    return;
  }
  wchar_t command[kMaxCommandLineChars] = {};
  (void)swprintf(command, kMaxCommandLineChars,
                 L"%s -o ClearAllForwardings=yes -s %s sftp", ssh_wide, host_wide);

  ChildStream child;
  const Status spawned = child.Spawn(ChildStreamOptions{command, nullptr, nullptr});
  if (spawned.has_value() == false) {
    Report("sftp-reachability", false, "CreateProcessW failed");
    return;
  }
  sftp::Client client(child.stream());
  const Status initialized = client.Init();
  if (initialized.has_value() == false) {
    char detail[128] = {};
    (void)std::snprintf(detail, sizeof(detail), "SFTP INIT failed (error=%u)",
                        static_cast<unsigned>(initialized.get_error()));
    Report("sftp-reachability", false, detail);
    child.Close();
    return;
  }
  const Result<sftp::Path> home = client.Realpath(".");
  char detail[512] = {};
  if (home.has_value()) {
    (void)std::snprintf(detail, sizeof(detail), "INIT ok, REALPATH . = %s", home.value().c_str());
    Report("sftp-reachability", true, detail);
  } else {
    (void)std::snprintf(detail, sizeof(detail), "REALPATH failed (error=%u)",
                        static_cast<unsigned>(home.get_error()));
    Report("sftp-reachability", false, detail);
  }
  child.Close();
}

// --- 8. memory baseline -----------------------------------------------------
void CheckMemory() noexcept {
  std::uint64_t working_set = 0;
  std::uint64_t private_bytes = 0;
  if (QueryMemory(&working_set, &private_bytes) == false) {
    Report("memory-baseline", false, "K32GetProcessMemoryInfo unavailable");
    return;
  }
  DWORD handles = 0;
  (void)GetProcessHandleCount(GetCurrentProcess(), &handles);
  char detail[256] = {};
  (void)std::snprintf(detail, sizeof(detail), "private=%llu KB working=%llu KB handles=%lu",
                      static_cast<unsigned long long>(private_bytes / 1024u),
                      static_cast<unsigned long long>(working_set / 1024u),
                      static_cast<unsigned long>(handles));
  Report("memory-baseline", true, detail);
}

}  // namespace

Status RunSelfTest(const SelfTestOptions& options) noexcept {
  PrintLine("picopaste selftest");
  PrintLine("-------------------");

  CheckClipboard();
  CheckWicEncode();
  CheckSendInput();
  CheckHotkey(options.config);
  CheckSingleInstance();
  CheckJobObjects(options.config);
  CheckSftp(options.config);
  CheckMemory();
  if (options.e2e) {
    ReportSkip("e2e-upload", "not implemented in this build");
  }

  char summary[128] = {};
  (void)std::snprintf(summary, sizeof(summary), "summary: %d passed, %d failed", g_passed, g_failed);
  PrintLine(summary);
  if (g_failed == 0) {
    return Status::success();
  }
  return Status::error(Error::kBusy);
}

}  // namespace picopaste::win32

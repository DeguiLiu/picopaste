// picopaste — platform-abstracted process memory snapshot.
//
// The design requires the process's memory to be observable under a hard cap
// (a Job Object on Windows). Only the counters the design names are exposed;
// this is not a monitoring framework.
//
// Seam contract:
//   MemorySnapshot SampleMemory() noexcept;
//   - must not allocate, must not throw, and must never block for long;
//   - fills what the platform exposes and sets valid=false on failure
//     (for example when /proc is unavailable);
//   - counters are zero when the platform cannot report them.
//
// Linux is implemented inline below (reads /proc/self/status and
// /proc/self/fd). On Windows the symbol is only declared here; the win32
// workstream provides the definition (GetProcessMemoryInfo +
// GetProcessHandleCount + GetProcessMemoryInfo.PeakWorkingSetSize).

#pragma once

#include <cstdint>

namespace picopaste {

struct MemorySnapshot {
  // Private committed bytes. Windows: PROCESS_MEMORY_COUNTERS.PrivateUsage.
  // Linux: VmData, the closest kernel-exposed private-data figure.
  std::uint64_t private_bytes = 0;

  // Resident working set. Windows: WorkingSetSize; Linux: resident pages.
  std::uint64_t working_set_bytes = 0;

  // Lifetime peak working set. Windows: PeakWorkingSetSize; Linux: VmHWM.
  std::uint64_t peak_bytes = 0;

  // Handles. Windows: handle count; Linux: open file descriptors.
  std::uint32_t handle_count = 0;

  std::uint32_t thread_count = 0;

  bool valid = false;
};

MemorySnapshot SampleMemory() noexcept;

}  // namespace picopaste

#if defined(__linux__)

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace picopaste {
namespace detail {

inline bool ReadProcFile(const char* path, char* buffer, std::size_t capacity) noexcept {
  const int fd = ::open(path, O_RDONLY);
  if (0 > fd) {
    return false;
  }
  std::size_t total = 0;
  bool ok = true;
  while ((total + 1U) < capacity) {
    const ssize_t n = ::read(fd, buffer + total, capacity - 1U - total);
    if (n > 0) {
      total += static_cast<std::size_t>(n);
      continue;
    }
    if (0 == n) {
      break;
    }
    if (EINTR == errno) {
      continue;
    }
    ok = false;
    break;
  }
  (void)::close(fd);
  buffer[total] = '\0';
  return ok;
}

// Parse "<key>: <number> kB" from /proc/self/status. Returns bytes.
inline std::uint64_t StatusKb(const char* status, const char* key) noexcept {
  const char* at = std::strstr(status, key);
  if (nullptr == at) {
    return 0;
  }
  at += std::strlen(key);
  while ((' ' == *at) || ('\t' == *at)) {
    ++at;
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(at, &end, 10);
  if (end == at) {
    return 0;
  }
  return static_cast<std::uint64_t>(value) * 1024ULL;
}

inline std::uint32_t StatusU32(const char* status, const char* key) noexcept {
  return static_cast<std::uint32_t>(StatusKb(status, key) / 1024ULL);
}

}  // namespace detail

inline MemorySnapshot SampleMemory() noexcept {
  MemorySnapshot snapshot;

  char status[4096];
  if (!detail::ReadProcFile("/proc/self/status", status, sizeof(status))) {
    return snapshot;
  }
  snapshot.private_bytes = detail::StatusKb(status, "VmData:");
  snapshot.thread_count = detail::StatusU32(status, "Threads:");

  // Working set and its peak MUST come from this one buffer. VmRSS is the
  // current resident set and VmHWM the kernel's high-water mark of that same
  // counter. Deriving the current value from a second file (/proc/self/statm)
  // at a later instant lets the process fault in pages between the two reads;
  // under ASan the resident set grew past the earlier VmHWM and the
  // peak >= working_set invariant broke. Reading both here keeps a single
  // sample internally consistent.
  const std::uint64_t working_set = detail::StatusKb(status, "VmRSS:");
  const std::uint64_t high_water = detail::StatusKb(status, "VmHWM:");
  snapshot.working_set_bytes = working_set;
  // The high-water mark can lag a just-grown resident set (the kernel folds an
  // increase into hiwater_rss lazily), and the current sample is by definition
  // part of the peak. Taking the larger of the two removes that lag without
  // reintroducing a second source.
  snapshot.peak_bytes = (high_water > working_set) ? high_water : working_set;

  // Linux analog of a handle count: open file descriptors.
  if (DIR* dir = ::opendir("/proc/self/fd")) {
    std::uint32_t count = 0;
    while (dirent* entry = ::readdir(dir)) {
      const char* name = entry->d_name;
      if ((0 == std::strcmp(name, ".")) || (0 == std::strcmp(name, ".."))) {
        continue;
      }
      ++count;
    }
    (void)::closedir(dir);
    snapshot.handle_count = count;
  }

  snapshot.valid = true;
  return snapshot;
}

}  // namespace picopaste

#endif  // __linux__

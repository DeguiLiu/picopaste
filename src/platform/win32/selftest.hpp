// picopaste -- per-capability self-check table.
//
// This is how the user verifies the build on their own machine. Every
// capability prints one PASS / FAIL / SKIP line with a concrete number (byte
// counts, enforced memory limits, resolved remote path), never a bare "ok".
#pragma once

#include <cstdint>

#include "picopaste/config.hpp"
#include "picopaste/error.hpp"

namespace picopaste::win32 {

struct SelfTestOptions {
  // Parsed configuration. Required.
  const Config* config = nullptr;
  // Reserved for the end-to-end upload check (M5); currently reports SKIP.
  bool e2e = false;
  // Port used for the single-instance mutex probe.
  std::uint16_t port = 0;
};

// Run the table to stdout. Returns kOk only when no mandatory capability failed.
Status RunSelfTest(const SelfTestOptions& options) noexcept;

}  // namespace picopaste::win32

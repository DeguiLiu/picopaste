// picopaste — fixed-allocation memory budget gate (design §5).
//
// The design promises a steady state ≤ 12 MB (hard Job Object cap 32 MB) and
// backs it with a discipline of fixed-capacity buffers and zero hot-path
// allocation. A full end-to-end memory figure cannot be reproduced honestly on
// Linux: the working set depends on the Windows CRT, the WIC/COM stack and the
// ssh child process, none of which exist here. What CAN be checked honestly is
// the part the project controls at compile time — the sizes of the fixed
// buffers whose growth is exactly what would blow the budget.
//
// This is deliberately narrow. It catches an accidental large static or a
// scratch buffer that doubles; it does not claim to measure runtime memory.
// The real 12 MB / 32 MB numbers come from the Windows `selftest` and the Job
// Object, per design §5 and §10.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include "../src/core/app/lifecycle.hpp"
#include "../src/core/log/log_ring.hpp"
#include "../src/core/log/log_sink.hpp"
#include "picopaste/config.hpp"
#include "picopaste/sftp/client.hpp"
#include "picopaste/sftp/protocol.hpp"

namespace {

// Design §5 budgets, in bytes. The per-object bounds come straight from the
// budget table; the sum bound is the total fixed per-instance footprint the
// core may hold. Measured on the current tree: Config 780, Client 131624,
// LogRing 23168, LogSink 536, Lifecycle 488 — 156596 bytes (152.9 KB) total.
//
// The SFTP client crossed its old 96 KB budget when all wire I/O was unified
// into it: ListDir must hold a READDIR NAME frame (~64 KB), and ReadFile reads
// DATA frames, so rx_ doubled from 16 KB to 64 KB. Client is now 66048 tx +
// 65536 rx + 40 bookkeeping = 131624 bytes (128.5 KB). The bounds below are set
// just above the measured values — Client 160 KB leaves ~24% headroom and the
// 192 KB aggregate ~25% — while still failing on a doubling of any large
// buffer.
constexpr std::size_t kConfigBudget = 16u * 1024u;         // design: ≤ 16 KB
constexpr std::size_t kLogRingBudget = 64u * 1024u;        // design: log ring 64 KB
constexpr std::size_t kSftpClientBudget = 160u * 1024u;    // measured 131624: 64 KB tx + 64 KB rx + slack
constexpr std::size_t kCoreFixedBudget = 192u * 1024u;     // measured sum 156596, ~25% headroom

}  // namespace

TEST_CASE("core fixed buffers stay inside the design's memory budget") {
  const std::size_t total = sizeof(picopaste::Config) +
                            sizeof(picopaste::sftp::Client) +
                            sizeof(picopaste::LogRing) +
                            sizeof(picopaste::LogSink) +
                            sizeof(picopaste::Lifecycle);

  INFO("Config=" << sizeof(picopaste::Config)
                 << " Client=" << sizeof(picopaste::sftp::Client)
                 << " LogRing=" << sizeof(picopaste::LogRing)
                 << " LogSink=" << sizeof(picopaste::LogSink)
                 << " Lifecycle=" << sizeof(picopaste::Lifecycle)
                 << " total=" << total);

  // Config is read once, never a heap string: growth here means an unbounded
  // field slipped in.
  CHECK(kConfigBudget >= sizeof(picopaste::Config));
  // The log ring is a fixed array of per-producer SPSC rings.
  CHECK(sizeof(picopaste::LogRing) <= kLogRingBudget);
  // The SFTP client's tx_/rx_ scratch is the single largest fixed buffer; the
  // design calls it out specifically (~128 KB) as the reason Client must not
  // live on a 256 KB worker stack.
  CHECK(sizeof(picopaste::sftp::Client) >= picopaste::sftp::kWriteChunkBytes);
  CHECK(sizeof(picopaste::sftp::Client) <= kSftpClientBudget);
  // Aggregate ceiling: a doubling of any large buffer trips this.
  CHECK(total <= kCoreFixedBudget);
}

TEST_CASE("wire constants stay within their documented bounds") {
  // The outbound chunk is the hot-path I/O allocation; the design budgets
  // exactly one 64 KB chunk, reused.
  CHECK(picopaste::sftp::kWriteChunkBytes == 64u * 1024u);
  // A path longer than this is rejected, not truncated (client.hpp).
  CHECK(picopaste::sftp::kMaxPathBytes == 512u);
  // One log record stays well under 256 B so a 64 KB ring holds depth records.
  CHECK(sizeof(picopaste::LogRecord) <= 256u);
}

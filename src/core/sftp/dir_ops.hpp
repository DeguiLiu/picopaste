/**
 * MIT License
 *
 * Copyright (c) 2026 liudegui
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file dir_ops.hpp
 * @brief Upload-directory retention policy.
 *
 * The remote has no shell, so bounding /tmp/picopaste is expressed
 * purely as a decision over a directory listing. The listing itself is produced
 * by sftp::Client::ListDir (the single owner of the wire protocol); this layer
 * keeps only the pure policy and a thin driver that applies it through Client.
 *
 * Memory discipline: a listing is materialised by Client into fixed-capacity
 * containers (kMaxListedEntries). If the cap is hit the listing is marked
 * `truncated` and PlanRetention refuses to delete anything — a cleanup must
 * never act on an incomplete view of the directory.
 */
#pragma once

#include "osp/vocabulary.hpp"
#include "picopaste/error.hpp"
#include "picopaste/sftp/client.hpp"  // Client, UploadListing, kMaxListedEntries

#include <cstdint>

namespace picopaste::sftp {

// Age and count limits. Zero disables the corresponding rule.
struct RetentionPolicy {
  std::uint64_t max_age_seconds = 0u;  // delete entries older than this.
  std::uint32_t keep_newest = 0u;      // keep this many newest; delete the rest.
};

// Pure decision output: indices into UploadListing::entries to delete.
struct RetentionPlan {
  osp::FixedVector<std::uint32_t, kMaxListedEntries> remove_indices{};
  std::uint32_t kept_unknown_mtime = 0u;  // entries with no ATTRS time: never deleted.
  bool incomplete = false;                // input truncated: caller must not delete.
};

// Summary of one cleanup pass.
struct CleanupResult {
  std::uint32_t listed = 0u;  // matching entries seen.
  std::uint32_t removed = 0u;
  std::uint32_t failed = 0u;   // REMOVE returned a non-tolerated error.
  std::uint32_t skipped = 0u;  // foreign names left alone.
  bool truncated = false;      // listing cap hit: nothing was deleted.
};

/**
 * @brief Decide which listed entries to delete.
 * @param listing Snapshot from Client::ListDir.
 * @param now_unix Current Unix time, compared against each entry's mtime.
 * @param policy Age and count limits; zero disables a rule.
 * @return Indices into `listing.entries` to remove, flagged `incomplete`
 *         (with no removals) when the listing was truncated.
 *
 * Pure and I/O-free: a function of the listing and the current time.
 */
RetentionPlan PlanRetention(const UploadListing& listing, std::uint64_t now_unix,
                            const RetentionPolicy& policy) noexcept;

// Thin driver: list through the shared Client, apply the pure policy, then
// remove the selected entries through the same Client. Holds no wire state of
// its own — it borrows the caller's already-INIT'ed client.
class DirOps {
 public:
  DirOps(const DirOps&) = delete;
  DirOps& operator=(const DirOps&) = delete;
  DirOps(DirOps&&) = delete;
  DirOps& operator=(DirOps&&) = delete;

  /**
   * @brief Construct over an already-INIT'ed channel.
   * @param client The Client that performs the listing and removals; must
   *        outlive this object.
   */
  explicit DirOps(Client& client) noexcept : client_(client) {}

  /**
   * @brief List `dir`, apply the pure policy, then remove the selected entries.
   * @param dir Remote directory to clean.
   * @param now_unix Current Unix time for the age rule.
   * @param policy Age and count limits; zero disables a rule.
   * @return Per-pass counts, or an error from listing or removing.
   *
   * A missing directory is a successful no-op.
   */
  Result<CleanupResult> Cleanup(const char* dir, std::uint64_t now_unix, const RetentionPolicy& policy) noexcept;

 private:
  Client& client_;
};

}  // namespace picopaste::sftp

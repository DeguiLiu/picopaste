// picopaste — upload-directory retention policy.
//
// The remote has no shell, so bounding /tmp/picopaste is expressed
// purely as a decision over a directory listing. The listing itself is produced
// by sftp::Client::ListDir (the single owner of the wire protocol); this layer
// keeps only the pure policy and a thin driver that applies it through Client.
//
// Memory discipline: a listing is materialised by Client into fixed-capacity
// containers (kMaxListedEntries). If the cap is hit the listing is marked
// `truncated` and PlanRetention refuses to delete anything — a cleanup must
// never act on an incomplete view of the directory.
#pragma once

#include <cstdint>

#include "picopaste/error.hpp"
#include "picopaste/sftp/client.hpp"  // Client, UploadListing, kMaxListedEntries
#include "osp/vocabulary.hpp"

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
  std::uint32_t listed = 0u;   // matching entries seen.
  std::uint32_t removed = 0u;
  std::uint32_t failed = 0u;   // REMOVE returned a non-tolerated error.
  std::uint32_t skipped = 0u;  // foreign names left alone.
  bool truncated = false;      // listing cap hit: nothing was deleted.
};

// Pure retention decision: a function of the listing and the current time,
// with no I/O. When `listing.truncated` is set the result is flagged
// `incomplete` and carries no removals.
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

  // `client` must be the already-INIT'ed channel and must outlive this object.
  explicit DirOps(Client& client) noexcept : client_(client) {}

  // List `dir`, apply the pure policy, then remove the selected entries. A
  // missing directory is a successful no-op.
  Result<CleanupResult> Cleanup(const char* dir, std::uint64_t now_unix,
                                const RetentionPolicy& policy) noexcept;

 private:
  Client& client_;
};

}  // namespace picopaste::sftp

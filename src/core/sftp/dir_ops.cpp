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
 * @file dir_ops.cpp
 * @brief Upload-directory retention policy implementation.
 *
 * No wire code lives here: listing and removal are delegated to sftp::Client,
 * the single owner of the SFTP channel. What remains is the pure retention
 * decision and the driver that applies it.
 */
#include "dir_ops.hpp"

#include <cstring>

namespace picopaste::sftp {
namespace {

// Orders two matched entries newest-first. Equal mtimes (all uploads in the
// same second) fall back to the name, so "keep newest M" stays deterministic.
bool NameGreater(const osp::FixedString<kMaxUploadNameBytes>& a,
                 const osp::FixedString<kMaxUploadNameBytes>& b) noexcept {
  const std::uint32_t an = a.size();
  const std::uint32_t bn = b.size();
  const std::uint32_t n = (an < bn) ? an : bn;
  const std::int32_t cmp = std::memcmp(a.c_str(), b.c_str(), n);
  if (cmp != 0) {
    return cmp > 0;
  }
  return an > bn;
}

bool NewerFirst(const DirEntry& a, const DirEntry& b) noexcept {
  if (a.mtime != b.mtime) {
    return a.mtime > b.mtime;
  }
  return NameGreater(a.name, b.name);
}

// A name can be selected by both the age rule and the keep-newest rule. Each
// index must appear once so a file is not removed twice: the second REMOVE
// would answer NO_SUCH_FILE, which Remove tolerates, but the plan would
// overstate the work and the pass would report a failure that never happened.
void AddUnique(RetentionPlan& plan, std::uint32_t index) noexcept {
  for (std::uint32_t i = 0u; i < plan.remove_indices.size(); ++i) {
    if (plan.remove_indices[i] == index) {
      return;
    }
  }
  (void)plan.remove_indices.push_back(index);
}

// Copies `dir` + "/" + `name` into `out`; false if it would not fit.
bool JoinPath(const char* dir, const char* name, char* out) noexcept {
  const std::size_t dir_len = std::strlen(dir);
  const std::size_t name_len = std::strlen(name);
  const bool needs_slash = (dir_len > 0u) && (dir[dir_len - 1u] != '/');
  const std::size_t total = dir_len + (needs_slash ? 1u : 0u) + name_len;
  if (kMaxPathBytes < (total + 1u)) {
    return false;
  }
  (void)std::memcpy(out, dir, dir_len);
  std::size_t pos = dir_len;
  if (needs_slash) {
    out[pos++] = '/';
  }
  (void)std::memcpy(out + pos, name, name_len);
  out[pos + name_len] = '\0';
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure retention policy
// ---------------------------------------------------------------------------

RetentionPlan PlanRetention(const UploadListing& listing, std::uint64_t now_unix,
                            const RetentionPolicy& policy) noexcept {
  RetentionPlan plan{};
  if (listing.truncated) {
    plan.incomplete = true;
    return plan;
  }
  const std::uint32_t n = listing.entries.size();
  for (std::uint32_t i = 0u; i < n; ++i) {
    if (!listing.entries[i].has_mtime) {
      ++plan.kept_unknown_mtime;
    }
  }

  if (policy.max_age_seconds > 0u) {
    for (std::uint32_t i = 0u; i < n; ++i) {
      const DirEntry& e = listing.entries[i];
      if (e.has_mtime && (now_unix >= e.mtime) && ((now_unix - e.mtime) > policy.max_age_seconds)) {
        AddUnique(plan, i);
      }
    }
  }

  if (policy.keep_newest > 0u) {
    osp::FixedVector<std::uint32_t, kMaxListedEntries> order{};
    for (std::uint32_t i = 0u; i < n; ++i) {
      if (listing.entries[i].has_mtime) {
        (void)order.push_back(i);
      }
    }
    /* Insertion sort, newest first; n <= kMaxListedEntries. */
    for (std::uint32_t i = 1u; i < order.size(); ++i) {
      const std::uint32_t key = order[i];
      std::uint32_t j = i;
      while ((j > 0u) && NewerFirst(listing.entries[key], listing.entries[order[j - 1u]])) {
        order[j] = order[j - 1u];
        --j;
      }
      order[j] = key;
    }
    const std::uint32_t keep = (policy.keep_newest < order.size()) ? policy.keep_newest : order.size();
    for (std::uint32_t k = keep; k < order.size(); ++k) {
      AddUnique(plan, order[k]);
    }
  }
  return plan;
}

// ---------------------------------------------------------------------------
// Cleanup = List + PlanRetention + Remove
// ---------------------------------------------------------------------------

Result<CleanupResult> DirOps::Cleanup(const char* dir, std::uint64_t now_unix, const RetentionPolicy& policy) noexcept {
  CleanupResult res{};
  const Result<UploadListing> listed = client_.ListDir(dir);
  if (!listed) {
    return Result<CleanupResult>::error(listed.get_error());
  }
  const UploadListing& listing = listed.value();
  res.skipped = listing.skipped;
  if (listing.dir_missing || listing.truncated) {
    res.truncated = listing.truncated;
    return Result<CleanupResult>::success(res);
  }

  res.listed = listing.entries.size();
  const RetentionPlan plan = PlanRetention(listing, now_unix, policy);
  if (plan.incomplete) {
    res.truncated = true;
    return Result<CleanupResult>::success(res);
  }

  for (std::uint32_t k = 0u; k < plan.remove_indices.size(); ++k) {
    const std::uint32_t idx = plan.remove_indices[k];
    if (idx >= listing.entries.size()) {
      ++res.failed;
      continue;
    }
    char path[kMaxPathBytes];
    if (!JoinPath(dir, listing.entries[idx].name.c_str(), path)) {
      ++res.failed;
      continue;
    }
    const Status removed = client_.Remove(path);
    if (removed) {
      ++res.removed;
    } else {
      ++res.failed;
    }
  }
  return Result<CleanupResult>::success(res);
}

}  // namespace picopaste::sftp

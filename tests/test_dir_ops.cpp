// picopaste — directory-listing / retention tests.
//
// Two layers, mirroring test_sftp_client.cpp:
//   1. Pure unit tests for MatchesUploadName and PlanRetention. These need no
//      channel and pin the destructive decisions exactly.
//   2. A real end-to-end test over `ssh -s localhost sftp` against this
//      machine's own sftp-server: create a directory with matching and
//      non-matching names, list it, and run the age and count policies. Safe
//      cleanup is the whole point, so the load-bearing assertion is that a
//      non-matching file is never removed.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "test_support.hpp"

#include "picopaste/sftp/client.hpp"
#include "picopaste/sftp/protocol.hpp"
#include "../src/core/sftp/dir_ops.hpp"

using namespace picopaste;
using namespace picopaste::sftp;

namespace {

// ---------------------------------------------------------------------------
// Pure-test helpers
// ---------------------------------------------------------------------------

DirEntry MakeEntry(const char* name, std::uint32_t mtime, bool has_mtime = true) {
  DirEntry e{};
  e.name.assign(osp::TruncateToCapacity, name);
  e.mtime = mtime;
  e.has_mtime = has_mtime;
  return e;
}

bool HasEntry(const UploadListing& listing, const char* name) {
  for (std::uint32_t i = 0u; i < listing.entries.size(); ++i) {
    if (std::strcmp(listing.entries[i].name.c_str(), name) == 0) {
      return true;
    }
  }
  return false;
}

bool PlanRemoves(const RetentionPlan& plan, const UploadListing& listing, const char* name) {
  for (std::uint32_t k = 0u; k < plan.remove_indices.size(); ++k) {
    const std::uint32_t idx = plan.remove_indices[k];
    if ((idx < listing.entries.size()) &&
        (std::strcmp(listing.entries[idx].name.c_str(), name) == 0)) {
      return true;
    }
  }
  return false;
}

bool Match(const char* s) {
  return MatchesUploadName(s, static_cast<std::uint32_t>(std::strlen(s)));
}

bool NoMatch(const char* s) {
  return !MatchesUploadName(s, static_cast<std::uint32_t>(std::strlen(s)));
}

// ---------------------------------------------------------------------------
// Integration plumbing
// ---------------------------------------------------------------------------

struct Session {
  ByteStream b{};
  Client client;
  DirOps dir;

  explicit Session(ByteStream stream) noexcept : b(stream), client(b), dir(client) {}
  ~Session() {
    if (b.valid()) {
      b.close(b.ctx);
    }
  }
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
};

std::unique_ptr<Session> ConnectSftp() {
  const char* argv[] = {"ssh",
                        "-o",
                        "ClearAllForwardings=yes",
                        "-o",
                        "BatchMode=yes",
                        "-o",
                        "LogLevel=ERROR",
                        "-s",
                        "localhost",
                        "sftp",
                        nullptr};
  auto spawned = test::SpawnStream(argv);
  if (!spawned.has_value()) {
    return nullptr;
  }
  auto session = std::make_unique<Session>(spawned.value());
  if (!session->client.Init()) {
    return nullptr;
  }
  return session;
}

// Creates a one-byte local file reused for every upload.
test::TempFile MakeLocalFile()
{
  const std::uint8_t payload = static_cast<std::uint8_t>('x');
  return test::TempFile::Create(&payload, 1u);
}

}  // namespace

// ---------------------------------------------------------------------------
// MatchesUploadName
// ---------------------------------------------------------------------------

TEST_CASE("MatchesUploadName accepts exactly our upload shape", "[dirops][pattern]") {
  CHECK(Match("clip-20240101-010101-abcdef.png"));
  CHECK(Match("clip-20240101-010101-a.png")); /* 1 hex digit */
  CHECK(Match("clip-20261231-235959-0123456789abcdef.png"));
  /* 64 hex digits is the maximum. */
  const std::string max_hex = std::string("clip-20240101-010101-") + std::string(64u, 'a') + ".png";
  CHECK(max_hex.size() == kMaxUploadNameBytes);
  CHECK(Match(max_hex.c_str()));
}

TEST_CASE("MatchesUploadName rejects every near miss", "[dirops][pattern]") {
  CHECK(NoMatch("keepme.txt"));
  CHECK(NoMatch("photo.png"));
  CHECK(NoMatch("clip-20240101-010101-ABCDEF.png"));  /* uppercase hex */
  CHECK(NoMatch("CLIP-20240101-010101-abcdef.png"));  /* uppercase prefix */
  CHECK(NoMatch("clip-20240101-01010-abcdef.png"));   /* short time */
  CHECK(NoMatch("clip-20241301-010101-abcdef.png"));  /* month 13 */
  CHECK(NoMatch("clip-20240132-010101-abcdef.png"));  /* day 32 */
  CHECK(NoMatch("clip-20240101-240101-abcdef.png"));  /* hour 24 */
  CHECK(NoMatch("clip-20240101-010101-.png"));        /* no hex */
  CHECK(NoMatch("clip-20240101-010101-abcdef.png.bak")); /* trailing */
  CHECK(NoMatch("clip-2024010-010101-abcdef.png"));   /* short date */
  CHECK(NoMatch("clip-20240101-010101-abcdef.jpg"));  /* wrong ext */
  /* A prefix of a valid name must not match (full match required). */
  CHECK(NoMatch("clip-20240101-010101-abcdef"));
  CHECK(NoMatch(""));
  const std::string too_many_hex =
      std::string("clip-20240101-010101-") + std::string(65u, 'a') + ".png";
  CHECK(NoMatch(too_many_hex.c_str()));
}

// ---------------------------------------------------------------------------
// PlanRetention
// ---------------------------------------------------------------------------

TEST_CASE("PlanRetention age rule removes only entries older than the limit", "[dirops][policy]") {
  UploadListing listing{};
  (void)listing.entries.push_back(MakeEntry("old.png", 1000u));
  (void)listing.entries.push_back(MakeEntry("edge.png", 6000u));
  (void)listing.entries.push_back(MakeEntry("new.png", 9000u));

  RetentionPolicy policy{};
  policy.max_age_seconds = 5000u; /* now - mtime > 5000 */
  const RetentionPlan plan = PlanRetention(listing, 10000u, policy);

  CHECK_FALSE(plan.incomplete);
  CHECK(plan.remove_indices.size() == 1u);
  CHECK(PlanRemoves(plan, listing, "old.png"));
  CHECK_FALSE(PlanRemoves(plan, listing, "edge.png"));
  CHECK_FALSE(PlanRemoves(plan, listing, "new.png"));
}

TEST_CASE("PlanRetention count rule keeps exactly the newest M", "[dirops][policy]") {
  UploadListing listing{};
  (void)listing.entries.push_back(MakeEntry("a", 1u));
  (void)listing.entries.push_back(MakeEntry("b", 2u));
  (void)listing.entries.push_back(MakeEntry("c", 3u));
  (void)listing.entries.push_back(MakeEntry("d", 4u));
  (void)listing.entries.push_back(MakeEntry("e", 5u));

  RetentionPolicy policy{};
  policy.keep_newest = 2u;
  const RetentionPlan plan = PlanRetention(listing, 100u, policy);

  CHECK(plan.remove_indices.size() == 3u);
  CHECK(PlanRemoves(plan, listing, "a"));
  CHECK(PlanRemoves(plan, listing, "b"));
  CHECK(PlanRemoves(plan, listing, "c"));
  CHECK_FALSE(PlanRemoves(plan, listing, "d"));
  CHECK_FALSE(PlanRemoves(plan, listing, "e"));
}

TEST_CASE("PlanRetention count rule breaks mtime ties by name", "[dirops][policy]") {
  UploadListing listing{};
  (void)listing.entries.push_back(MakeEntry("a", 7u));
  (void)listing.entries.push_back(MakeEntry("b", 7u));
  (void)listing.entries.push_back(MakeEntry("c", 7u));

  RetentionPolicy policy{};
  policy.keep_newest = 1u;
  const RetentionPlan plan = PlanRetention(listing, 100u, policy);

  CHECK(plan.remove_indices.size() == 2u);
  CHECK(PlanRemoves(plan, listing, "a"));
  CHECK(PlanRemoves(plan, listing, "b"));
  CHECK_FALSE(PlanRemoves(plan, listing, "c"));
}

TEST_CASE("PlanRetention combines rules without double counting", "[dirops][policy]") {
  UploadListing listing{};
  (void)listing.entries.push_back(MakeEntry("old", 1u));   /* expired and not newest */
  (void)listing.entries.push_back(MakeEntry("mid", 5u));
  (void)listing.entries.push_back(MakeEntry("new", 9u));

  RetentionPolicy policy{};
  policy.max_age_seconds = 6u; /* removes "old" only (now-mtime 9 > 6) */
  policy.keep_newest = 2u;     /* also would remove "old" */
  const RetentionPlan plan = PlanRetention(listing, 10u, policy);

  CHECK(plan.remove_indices.size() == 1u); /* deduplicated */
  CHECK(PlanRemoves(plan, listing, "old"));
}

TEST_CASE("PlanRetention never deletes an entry with unknown mtime", "[dirops][policy]") {
  UploadListing listing{};
  (void)listing.entries.push_back(MakeEntry("known", 1u));
  (void)listing.entries.push_back(MakeEntry("unknown", 0u, false));

  RetentionPolicy policy{};
  policy.max_age_seconds = 1u;
  policy.keep_newest = 1u;
  const RetentionPlan plan = PlanRetention(listing, 1000u, policy);

  CHECK(plan.kept_unknown_mtime == 1u);
  CHECK_FALSE(PlanRemoves(plan, listing, "unknown"));
  CHECK(PlanRemoves(plan, listing, "known"));
}

TEST_CASE("PlanRetention refuses to act on a truncated listing", "[dirops][policy]") {
  UploadListing listing{};
  (void)listing.entries.push_back(MakeEntry("a", 1u));
  listing.truncated = true;

  RetentionPolicy policy{};
  policy.max_age_seconds = 1u;
  const RetentionPlan plan = PlanRetention(listing, 1000u, policy);

  CHECK(plan.incomplete);
  CHECK(plan.remove_indices.size() == 0u);
}

TEST_CASE("PlanRetention handles a cap-sized listing", "[dirops][policy]") {
  UploadListing listing{};
  for (std::uint32_t i = 0u; i < kMaxListedEntries; ++i) {
    char name[16] = {};
    (void)std::snprintf(name, sizeof(name), "f%05u", static_cast<unsigned>(i));
    (void)listing.entries.push_back(MakeEntry(name, i + 1u));
  }
  REQUIRE(listing.entries.size() == kMaxListedEntries);

  RetentionPolicy policy{};
  policy.keep_newest = 100u;
  const RetentionPlan plan = PlanRetention(listing, 100000u, policy);
  CHECK(plan.remove_indices.size() == (kMaxListedEntries - 100u));
}

// ---------------------------------------------------------------------------
// Real end-to-end against the machine's own sftp-server
// ---------------------------------------------------------------------------

TEST_CASE("end-to-end listing, pattern safety and age cleanup", "[dirops][integration]") {
  auto session = ConnectSftp();
  if (session == nullptr) {
    SKIP("local sftp subsystem unavailable");
  }
  Client& c = session->client;
  DirOps& d = session->dir;

  const auto home = c.Realpath(".");
  REQUIRE(home.has_value());
  const std::string base(home.value().c_str());
  const std::string pid = std::to_string(test::ProcessId());
  const std::string dir = base + "/.cache/picopaste-dirops-" + pid;
  REQUIRE(c.MkdirAll(dir.c_str()));

  const test::TempFile local = MakeLocalFile();
  REQUIRE(local.valid());

  const char* kMatch[] = {
      "clip-20240101-010101-aaaa0001.png",
      "clip-20240101-010102-aaaa0002.png",
      "clip-20240101-010103-aaaa0003.png",
      "clip-20240101-010104-aaaa0004.png",
  };
  const char* kForeign[] = {
      "keepme.txt",
      "photo.png",
      "clip-20240101-01010-aaaa.png",   /* short time */
      "CLIP-20240101-010101-aaaa.png",  /* uppercase prefix */
      "clip-20240101-010101-AAAA.png",  /* uppercase hex */
      "clip-20241301-010101-aaaa.png",  /* impossible month */
  };
  for (const char* name : kMatch) {
    const std::string remote = dir + "/" + name;
    REQUIRE(c.UploadFile(remote.c_str(), local.path()));
  }
  for (const char* name : kForeign) {
    const std::string remote = dir + "/" + name;
    REQUIRE(c.UploadFile(remote.c_str(), local.path()));
  }

  const auto listing = c.ListDir(dir.c_str());
  REQUIRE(listing.has_value());
  CHECK_FALSE(listing.value().dir_missing);
  CHECK_FALSE(listing.value().truncated);
  CHECK(listing.value().entries.size() == 4u);
  /* "." and ".." plus the six foreign files are all foreign to this pattern. */
  CHECK(listing.value().skipped >= 6u);
  for (std::uint32_t i = 0u; i < listing.value().entries.size(); ++i) {
    CHECK(MatchesUploadName(listing.value().entries[i].name.c_str(),
                            listing.value().entries[i].name.size()));
  }
  for (const char* name : kForeign) {
    CHECK_FALSE(HasEntry(listing.value(), name));
  }

  /* Age rule: pretend "now" is far in the future so every upload is expired. */
  const std::uint64_t far_future = static_cast<std::uint64_t>(std::time(nullptr)) + (400ull * 86400ull);
  RetentionPolicy age{};
  age.max_age_seconds = 30ull * 86400ull;
  const auto cleaned = d.Cleanup(dir.c_str(), far_future, age);
  REQUIRE(cleaned.has_value());
  CHECK(cleaned.value().removed == 4u);
  CHECK(cleaned.value().failed == 0u);
  CHECK_FALSE(cleaned.value().truncated);
  CHECK(cleaned.value().skipped >= 6u);

  const auto after = c.ListDir(dir.c_str());
  REQUIRE(after.has_value());
  CHECK(after.value().entries.size() == 0u);
  for (const char* name : kMatch) {
    const std::string remote = dir + "/" + name;
    CHECK_FALSE(c.StatSize(remote.c_str()).has_value());
  }
  /* The load-bearing assertion: foreign files survive. */
  for (const char* name : kForeign) {
    const std::string remote = dir + "/" + name;
    CHECK(c.StatSize(remote.c_str()).has_value());
  }

  /* Idempotent second run. */
  const auto again = d.Cleanup(dir.c_str(), far_future, age);
  REQUIRE(again.has_value());
  CHECK(again.value().removed == 0u);

  std::printf("[integration] dir=%s listed=4 removed=4 skipped=%u foreign_survived=6\n", dir.c_str(),
              static_cast<unsigned>(cleaned.value().skipped));
  SUCCEED("real listing, pattern filtering and age cleanup executed");
}

TEST_CASE("end-to-end count policy, idempotency, missing dir and vanished remove",
          "[dirops][integration]") {
  auto session = ConnectSftp();
  if (session == nullptr) {
    SKIP("local sftp subsystem unavailable");
  }
  Client& c = session->client;
  DirOps& d = session->dir;

  const auto home = c.Realpath(".");
  REQUIRE(home.has_value());
  const std::string base(home.value().c_str());
  const std::string pid = std::to_string(test::ProcessId());
  const std::string dir = base + "/.cache/picopaste-dirops-count-" + pid;
  REQUIRE(c.MkdirAll(dir.c_str()));

  const test::TempFile local = MakeLocalFile();
  REQUIRE(local.valid());

  const char* kMatch[] = {
      "clip-20240201-000001-bbbb0001.png",
      "clip-20240201-000002-bbbb0002.png",
      "clip-20240201-000003-bbbb0003.png",
      "clip-20240201-000004-bbbb0004.png",
  };
  for (const char* name : kMatch) {
    const std::string remote = dir + "/" + name;
    REQUIRE(c.UploadFile(remote.c_str(), local.path()));
  }
  const std::string foreign = dir + "/keepme.txt";
  REQUIRE(c.UploadFile(foreign.c_str(), local.path()));

  /* Count rule: keep the two newest, delete the rest. */
  RetentionPolicy count{};
  count.keep_newest = 2u;
  const std::uint64_t now = static_cast<std::uint64_t>(std::time(nullptr));
  const auto cleaned = d.Cleanup(dir.c_str(), now, count);
  REQUIRE(cleaned.has_value());
  CHECK(cleaned.value().removed == 2u);
  CHECK(cleaned.value().failed == 0u);

  const auto after = c.ListDir(dir.c_str());
  REQUIRE(after.has_value());
  CHECK(after.value().entries.size() == 2u);
  CHECK(HasEntry(after.value(), kMatch[3]));
  CHECK(HasEntry(after.value(), kMatch[2]));
  CHECK_FALSE(HasEntry(after.value(), kMatch[0]));
  CHECK_FALSE(HasEntry(after.value(), kMatch[1]));
  CHECK(c.StatSize(foreign.c_str()).has_value());

  const auto again = d.Cleanup(dir.c_str(), now, count);
  REQUIRE(again.has_value());
  CHECK(again.value().removed == 0u);

  /* Missing directory is a no-op, not an error. */
  const std::string absent = dir + "/does-not-exist";
  const auto missing = d.Cleanup(absent.c_str(), now, count);
  REQUIRE(missing.has_value());
  CHECK(missing.value().removed == 0u);
  CHECK(missing.value().listed == 0u);
  const auto missing_list = c.ListDir(absent.c_str());
  REQUIRE(missing_list.has_value());
  CHECK(missing_list.value().dir_missing);

  /* REMOVE of a vanished file (and a double remove) succeeds. */
  const std::string gone = dir + "/clip-20990101-000000-ffffffff.png";
  CHECK(c.Remove(gone.c_str()));
  CHECK(c.Remove(gone.c_str()));
  const std::string victim = dir + "/" + kMatch[3];
  CHECK(c.Remove(victim.c_str()));
  CHECK(c.Remove(victim.c_str()));

  std::printf("[integration] dir=%s count_removed=2 survivors=2 missing_dir=noop vanished_remove=ok\n",
              dir.c_str());
  SUCCEED("real count cleanup, idempotency, missing dir and vanished remove executed");
}

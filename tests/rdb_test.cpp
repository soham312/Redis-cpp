#include "persistence/rdb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "store/store.h"

using goredis::LoadRdb;
using goredis::SaveRdb;
using goredis::Store;

namespace {
// ::testing::TempDir() is GTest's cross-platform temp directory helper —
// used instead of a hardcoded /tmp so this suite behaves the same on any
// platform GTest itself supports.
std::string TestFilePath(const std::string& name) { return ::testing::TempDir() + "goredis_rdb_test_" + name + ".grdb"; }
}  // namespace

TEST(RdbTest, RoundTripPreservesStringsListsAndTtl) {
  std::string path = TestFilePath("roundtrip");
  {
    Store s;
    s.Set("str1", "hello world");
    s.Set("str2", "");  // empty string value — edge case worth covering explicitly
    s.RPush("list1", {"a", "b", "c"});
    s.Set("ttl_key", "expiring");
    ASSERT_TRUE(s.Expire("ttl_key", 100));
    ASSERT_TRUE(SaveRdb(s, path));
  }
  {
    Store loaded;
    loaded.Set("stale_key", "should be wiped by LoadSnapshot");
    ASSERT_TRUE(LoadRdb(loaded, path));

    EXPECT_EQ(loaded.Exists({"stale_key"}), 0);
    EXPECT_EQ(*loaded.Get("str1").value, "hello world");
    EXPECT_EQ(*loaded.Get("str2").value, "");
    EXPECT_EQ(*loaded.LRange("list1", 0, -1).value, (std::vector<std::string>{"a", "b", "c"}));
    long long ttl = loaded.TTL("ttl_key");
    EXPECT_GT(ttl, 0);
    EXPECT_LE(ttl, 100);
  }
}

TEST(RdbTest, SkipsKeyAlreadyExpiredAtSaveTime) {
  std::string path = TestFilePath("skip_expired");
  // Long sweep interval: the key below is logically expired but still
  // physically present when Snapshot() runs, directly exercising
  // Snapshot()'s own expiry check rather than relying on the background
  // sweeper having already removed it.
  Store s(std::chrono::milliseconds(10000));
  s.Set("about_to_expire", "v");
  ASSERT_TRUE(s.Expire("about_to_expire", 1));
  s.Set("survives", "v");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  ASSERT_TRUE(SaveRdb(s, path));

  Store loaded;
  ASSERT_TRUE(LoadRdb(loaded, path));
  EXPECT_EQ(loaded.Exists({"about_to_expire"}), 0);
  EXPECT_EQ(loaded.Exists({"survives"}), 1);
}

TEST(RdbTest, DowntimeExpiryIsAppliedAtLoadTimeNotSaveTime) {
  std::string path = TestFilePath("downtime");
  {
    Store s;
    s.Set("short_ttl", "v");
    ASSERT_TRUE(s.Expire("short_ttl", 1));  // 1 second remaining at save time
    s.Set("long_ttl", "v");
    ASSERT_TRUE(s.Expire("long_ttl", 3600));
    ASSERT_TRUE(SaveRdb(s, path));
  }

  // Simulate "the process was down for a while": real wall-clock time
  // passes between save and load, past short_ttl's deadline but nowhere
  // near long_ttl's. LoadRdb should treat short_ttl as having expired
  // during that gap (not resurrect it with a fresh countdown), while
  // long_ttl survives with its deadline essentially intact.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  Store loaded;
  ASSERT_TRUE(LoadRdb(loaded, path));
  EXPECT_EQ(loaded.Exists({"short_ttl"}), 0);
  EXPECT_EQ(loaded.Exists({"long_ttl"}), 1);
  long long ttl = loaded.TTL("long_ttl");
  EXPECT_GT(ttl, 3500);
  EXPECT_LE(ttl, 3600);
}

TEST(RdbTest, LoadingMissingFileFailsWithoutTouchingStore) {
  Store s;
  s.Set("untouched", "v");
  EXPECT_FALSE(LoadRdb(s, TestFilePath("does_not_exist")));
  EXPECT_EQ(s.Exists({"untouched"}), 1);
}

TEST(RdbTest, LoadingCorruptedFileFailsWithoutTouchingStore) {
  std::string path = TestFilePath("corrupt");
  {
    Store s;
    s.Set("k", "v");
    ASSERT_TRUE(SaveRdb(s, path));
  }
  {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    f.seekp(5);  // inside the payload, past the magic bytes
    char corrupt_byte = 0x7F;
    f.write(&corrupt_byte, 1);
  }

  Store loaded;
  loaded.Set("preexisting", "v");
  EXPECT_FALSE(LoadRdb(loaded, path));
  EXPECT_EQ(loaded.Exists({"preexisting"}), 1);
  EXPECT_EQ(loaded.Exists({"k"}), 0);  // the corrupt file's contents were never applied
}

TEST(RdbTest, EmptyStoreRoundTripsToEmptyAndClearsTarget) {
  std::string path = TestFilePath("empty");
  {
    Store s;
    ASSERT_TRUE(SaveRdb(s, path));
  }
  Store loaded;
  loaded.Set("will_be_cleared", "v");
  ASSERT_TRUE(LoadRdb(loaded, path));
  EXPECT_TRUE(loaded.Keys().empty());
}

TEST(RdbTest, RoundTripPreservesHashFields) {
  std::string path = TestFilePath("hash_roundtrip");
  {
    Store s;
    s.HSet("h", {{"f1", "v1"}, {"f2", "v2"}});
    s.HSet("empty_after_save", {{"f", "v"}});  // exercises a non-trivial field count on the wire
    ASSERT_TRUE(SaveRdb(s, path));
  }
  Store loaded;
  ASSERT_TRUE(LoadRdb(loaded, path));
  EXPECT_EQ(*loaded.HLen("h").value, 2);
  EXPECT_EQ(*loaded.HGet("h", "f1").value, "v1");
  EXPECT_EQ(*loaded.HGet("h", "f2").value, "v2");
  EXPECT_EQ(*loaded.HGet("empty_after_save", "f").value, "v");
}

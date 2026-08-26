#include "store/store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using goredis::Store;
using goredis::StoreError;

// ---------------------------------------------------------------------
// Basic string/key operations
// ---------------------------------------------------------------------

TEST(StoreTest, GetOnMissingKeyReturnsNotFound) {
  Store s;
  auto r = s.Get("nope");
  EXPECT_TRUE(r.Ok());
  EXPECT_FALSE(r.value.has_value());
}

TEST(StoreTest, SetThenGetRoundTrips) {
  Store s;
  s.Set("k", "v");
  auto r = s.Get("k");
  ASSERT_TRUE(r.Ok());
  ASSERT_TRUE(r.value.has_value());
  EXPECT_EQ(*r.value, "v");
}

TEST(StoreTest, SetOverwritesExistingValue) {
  Store s;
  s.Set("k", "v1");
  s.Set("k", "v2");
  EXPECT_EQ(*s.Get("k").value, "v2");
}

TEST(StoreTest, GetOnListKeyReturnsWrongType) {
  Store s;
  s.RPush("k", {"a"});
  auto r = s.Get("k");
  EXPECT_FALSE(r.Ok());
  EXPECT_EQ(r.error, StoreError::kWrongType);
}

TEST(StoreTest, DelRemovesKeysAndReportsCountActuallyRemoved) {
  Store s;
  s.Set("a", "1");
  s.Set("b", "2");
  EXPECT_EQ(s.Del({"a", "b", "nope"}), 2);
  EXPECT_EQ(s.Del({"a"}), 0);  // already gone
}

TEST(StoreTest, ExistsCountsDuplicateKeyArguments) {
  Store s;
  s.Set("a", "1");
  EXPECT_EQ(s.Exists({"a", "a", "nope"}), 2);
}

TEST(StoreTest, KeysReturnsEverySetKey) {
  Store s;
  s.Set("a", "1");
  s.Set("b", "2");
  auto keys = s.Keys();
  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(keys, (std::vector<std::string>{"a", "b"}));
}

TEST(StoreTest, FlushAllRemovesEverythingAndResetsUsedMemory) {
  Store s;
  s.Set("a", "1");
  s.RPush("b", {"x"});
  s.FlushAll();
  EXPECT_TRUE(s.Keys().empty());
  EXPECT_EQ(s.UsedMemory(), 0u);
}

// ---------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------

TEST(StoreTest, RPushAppendsInOrder) {
  Store s;
  auto r = s.RPush("list", {"a", "b", "c"});
  ASSERT_TRUE(r.Ok());
  EXPECT_EQ(*r.value, 3);
  EXPECT_EQ(*s.LRange("list", 0, -1).value, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(StoreTest, LPushPrependsSoLastArgEndsUpAtHead) {
  Store s;
  s.RPush("list", {"x", "y", "z"});
  auto r = s.LPush("list", {"w"});
  ASSERT_TRUE(r.Ok());
  EXPECT_EQ(*r.value, 4);
  EXPECT_EQ(*s.LRange("list", 0, -1).value, (std::vector<std::string>{"w", "x", "y", "z"}));
}

TEST(StoreTest, LLenOnMissingKeyIsZero) { EXPECT_EQ(*Store().LLen("nope").value, 0); }

TEST(StoreTest, LLenOnStringKeyIsWrongType) {
  Store s;
  s.Set("k", "v");
  EXPECT_FALSE(s.LLen("k").Ok());
}

TEST(StoreTest, LPushOnStringKeyIsWrongType) {
  Store s;
  s.Set("k", "v");
  EXPECT_FALSE(s.LPush("k", {"a"}).Ok());
}

TEST(StoreTest, LRangeSupportsNegativeIndices) {
  Store s;
  s.RPush("list", {"a", "b", "c", "d"});
  EXPECT_EQ(*s.LRange("list", -2, -1).value, (std::vector<std::string>{"c", "d"}));
}

TEST(StoreTest, LRangeClampsOutOfBoundsIndices) {
  Store s;
  s.RPush("list", {"a", "b"});
  EXPECT_EQ(*s.LRange("list", -100, 100).value, (std::vector<std::string>{"a", "b"}));
}

TEST(StoreTest, LRangeOnMissingKeyIsEmptyNotError) {
  Store s;
  auto r = s.LRange("nope", 0, -1);
  ASSERT_TRUE(r.Ok());
  EXPECT_TRUE(r.value->empty());
}

// ---------------------------------------------------------------------
// TTL / expiry
// ---------------------------------------------------------------------

TEST(StoreTest, TtlOnMissingKeyIsMinusTwo) { EXPECT_EQ(Store().TTL("nope"), -2); }

TEST(StoreTest, TtlOnKeyWithoutExpiryIsMinusOne) {
  Store s;
  s.Set("k", "v");
  EXPECT_EQ(s.TTL("k"), -1);
}

TEST(StoreTest, ExpireOnMissingKeyReturnsFalse) { EXPECT_FALSE(Store().Expire("nope", 10)); }

TEST(StoreTest, ExpireSetsApproximatelyCorrectTtl) {
  Store s;
  s.Set("k", "v");
  EXPECT_TRUE(s.Expire("k", 100));
  long long ttl = s.TTL("k");
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 100);
}

TEST(StoreTest, SetClearsAnyExistingTtl) {
  Store s;
  s.Set("k", "v");
  s.Expire("k", 100);
  s.Set("k", "v2");
  EXPECT_EQ(s.TTL("k"), -1);
}

TEST(StoreTest, NonPositiveExpireDeletesImmediately) {
  Store s;
  s.Set("k", "v");
  EXPECT_TRUE(s.Expire("k", 0));
  EXPECT_EQ(s.Exists({"k"}), 0);
}

TEST(StoreTest, LazyExpirationHidesKeyOnceDeadlinePasses) {
  Store s(std::chrono::milliseconds(10000));  // sweep interval far longer than the TTL below
  s.Set("k", "v");
  s.Expire("k", 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  EXPECT_FALSE(s.Get("k").value.has_value());
  EXPECT_EQ(s.Exists({"k"}), 0);
  EXPECT_EQ(s.TTL("k"), -2);
}

TEST(StoreTest, ActiveSweepReclaimsExpiredKeyWithoutBeingRead) {
  Store s(std::chrono::milliseconds(50));
  s.Set("k", "v");
  s.Expire("k", 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  // Deliberately no read of "k" before this check: if the background
  // sweeper (not lazy expiration on read) is what's supposed to reclaim
  // it, used memory has to already be back at zero without anything
  // having touched this specific key.
  EXPECT_EQ(s.UsedMemory(), 0u);
}

// ---------------------------------------------------------------------
// Memory accounting & eviction
// ---------------------------------------------------------------------

TEST(StoreTest, UsedMemoryStartsAtZeroAndGrowsWithInserts) {
  Store s;
  EXPECT_EQ(s.UsedMemory(), 0u);
  s.Set("k", "hello");
  EXPECT_GT(s.UsedMemory(), 0u);
}

TEST(StoreTest, UsedMemoryShrinksOnOverwriteWithSmallerValue) {
  Store s;
  s.Set("k", std::string(1000, 'x'));
  std::size_t big = s.UsedMemory();
  s.Set("k", "small");
  EXPECT_LT(s.UsedMemory(), big);
}

TEST(StoreTest, UsedMemoryReturnsToZeroAfterDeletingEverything) {
  Store s;
  s.Set("a", "1");
  s.Set("b", "2");
  s.Del({"a", "b"});
  EXPECT_EQ(s.UsedMemory(), 0u);
}

TEST(StoreTest, MaxMemoryZeroMeansUnlimited) {
  Store s;
  for (int i = 0; i < 500; ++i) {
    s.Set("k" + std::to_string(i), std::string(50, 'v'));
  }
  EXPECT_EQ(s.Keys().size(), 500u);
}

TEST(StoreTest, EvictionKeepsUsageUnderBudget) {
  Store s;
  for (int i = 0; i < 200; ++i) {
    s.Set("key" + std::to_string(i), std::string(40, 'v'));
  }
  std::size_t full_usage = s.UsedMemory();
  s.SetMaxMemory(full_usage / 4);

  EXPECT_LE(s.UsedMemory(), full_usage / 4);
  EXPECT_LT(s.Keys().size(), 200u);
  EXPECT_GT(s.Keys().size(), 0u);
}

TEST(StoreTest, WritesAfterLoweringBudgetStayUnderIt) {
  Store s;
  for (int i = 0; i < 200; ++i) {
    s.Set("key" + std::to_string(i), std::string(40, 'v'));
  }
  std::size_t budget = s.UsedMemory() / 4;
  s.SetMaxMemory(budget);
  for (int i = 200; i < 400; ++i) {
    s.Set("key" + std::to_string(i), std::string(40, 'v'));
    ASSERT_LE(s.UsedMemory(), budget);
  }
}

TEST(StoreTest, RecentlyTouchedKeysSurviveEvictionMoreOftenThanColdOnes) {
  Store s;
  for (int i = 0; i < 20; ++i) {
    s.Set("cold" + std::to_string(i), std::string(30, 'c'));
  }
  for (int i = 0; i < 10; ++i) {
    s.Set("hot" + std::to_string(i), std::string(30, 'h'));
  }
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 10; ++i) {
      s.Get("hot" + std::to_string(i));
    }
  }

  std::size_t full_usage = s.UsedMemory();
  s.SetMaxMemory(full_usage / 3);  // aggressive: evicts 2/3 of the keyspace in one shot

  int hot_survivors = 0;
  for (int i = 0; i < 10; ++i) {
    hot_survivors += s.Exists({"hot" + std::to_string(i)});
  }
  // Statistical, not exact: this is approximate LRU (see the design note
  // in store.h) sampling a small candidate set under extreme eviction
  // pressure, so a clean sweep isn't guaranteed. >=5 asserts "meaningfully
  // better than the ~3.3/10 a random policy would keep here" without
  // being brittle to the algorithm's inherent randomness.
  EXPECT_GE(hot_survivors, 5);
}

// ---------------------------------------------------------------------
// Snapshot / LoadSnapshot
// ---------------------------------------------------------------------

TEST(StoreTest, SnapshotAndLoadSnapshotRoundTrip) {
  Store s;
  s.Set("str", "hello");
  s.RPush("list", {"a", "b"});
  s.Set("ttlkey", "v");
  s.Expire("ttlkey", 100);

  auto snapshot = s.Snapshot();

  Store loaded;
  loaded.Set("stale", "v");
  loaded.LoadSnapshot(snapshot);

  EXPECT_EQ(loaded.Exists({"stale"}), 0);
  EXPECT_EQ(*loaded.Get("str").value, "hello");
  EXPECT_EQ(*loaded.LRange("list", 0, -1).value, (std::vector<std::string>{"a", "b"}));
  long long ttl = loaded.TTL("ttlkey");
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 100);
}

TEST(StoreTest, SnapshotExcludesLogicallyExpiredKeys) {
  Store s(std::chrono::milliseconds(10000));  // long sweep — key stays physically present past its deadline
  s.Set("expiring", "v");
  s.Expire("expiring", 1);
  s.Set("staying", "v");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  auto snapshot = s.Snapshot();
  bool found_staying = false;
  for (const auto& e : snapshot) {
    EXPECT_NE(e.key, "expiring");
    if (e.key == "staying") {
      found_staying = true;
    }
  }
  EXPECT_TRUE(found_staying);
}

TEST(StoreTest, LoadSnapshotDropsEntriesAlreadyExpiredByWallClock) {
  Store s;
  s.Set("shortlived", "v");
  s.Expire("shortlived", 1);
  auto snapshot = s.Snapshot();

  std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // simulate a gap before loading

  Store loaded;
  loaded.LoadSnapshot(snapshot);
  EXPECT_EQ(loaded.Exists({"shortlived"}), 0);
}

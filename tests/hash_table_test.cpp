#include "store/hash_table.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <utility>

using goredis::HashTable;

TEST(HashTableTest, GetOnEmptyReturnsNull) {
  HashTable<int> t;
  EXPECT_EQ(t.Get("missing"), nullptr);
}

TEST(HashTableTest, SetThenGetReturnsValue) {
  HashTable<int> t;
  EXPECT_FALSE(t.Set("a", 1));  // false: new insertion, not an update
  ASSERT_NE(t.Get("a"), nullptr);
  EXPECT_EQ(*t.Get("a"), 1);
}

TEST(HashTableTest, SetOnExistingKeyUpdatesAndReportsTrue) {
  HashTable<int> t;
  t.Set("a", 1);
  EXPECT_TRUE(t.Set("a", 2));  // true: this was an update
  EXPECT_EQ(*t.Get("a"), 2);
  EXPECT_EQ(t.Len(), 1u);
}

TEST(HashTableTest, DeletePresentKeyRemovesIt) {
  HashTable<int> t;
  t.Set("a", 1);
  EXPECT_TRUE(t.Delete("a"));
  EXPECT_EQ(t.Get("a"), nullptr);
  EXPECT_EQ(t.Len(), 0u);
}

TEST(HashTableTest, DeleteMissingKeyReturnsFalse) { EXPECT_FALSE(HashTable<int>().Delete("nope")); }

TEST(HashTableTest, LenTracksInsertsAndDeletes) {
  HashTable<int> t;
  for (int i = 0; i < 50; ++i) {
    t.Set("k" + std::to_string(i), i);
  }
  EXPECT_EQ(t.Len(), 50u);
  for (int i = 0; i < 20; ++i) {
    t.Delete("k" + std::to_string(i));
  }
  EXPECT_EQ(t.Len(), 30u);
}

TEST(HashTableTest, GrowsPastInitialCapacityWithoutLosingEntries) {
  HashTable<int> t;
  constexpr int kCount = 500;  // forces several resizes past the initial 16-bucket capacity
  for (int i = 0; i < kCount; ++i) {
    t.Set("key" + std::to_string(i), i);
  }
  ASSERT_EQ(t.Len(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    const int* v = t.Get("key" + std::to_string(i));
    ASSERT_NE(v, nullptr) << "key" << i;
    EXPECT_EQ(*v, i);
  }
}

TEST(HashTableTest, KeysReturnsEveryInsertedKeyExactlyOnce) {
  HashTable<int> t;
  std::set<std::string> expected;
  for (int i = 0; i < 30; ++i) {
    std::string k = "k" + std::to_string(i);
    t.Set(k, i);
    expected.insert(k);
  }
  auto keys = t.Keys();
  EXPECT_EQ(keys.size(), expected.size());
  std::set<std::string> actual(keys.begin(), keys.end());
  EXPECT_EQ(actual, expected);
}

TEST(HashTableTest, ForEachVisitsEveryEntryExactlyOnce) {
  HashTable<int> t;
  for (int i = 0; i < 30; ++i) {
    t.Set("k" + std::to_string(i), i);
  }
  int visit_count = 0;
  int sum = 0;
  t.ForEach([&](const std::string&, const int& v) {
    ++visit_count;
    sum += v;
  });
  EXPECT_EQ(visit_count, 30);
  EXPECT_EQ(sum, 29 * 30 / 2);
}

TEST(HashTableTest, ClearEmptiesTheTable) {
  HashTable<int> t;
  for (int i = 0; i < 30; ++i) {
    t.Set("k" + std::to_string(i), i);
  }
  t.Clear();
  EXPECT_EQ(t.Len(), 0u);
  EXPECT_EQ(t.Get("k0"), nullptr);
  EXPECT_TRUE(t.Keys().empty());
}

TEST(HashTableTest, ClearThenReinsertWorksNormally) {
  HashTable<int> t;
  t.Set("a", 1);
  t.Clear();
  t.Set("b", 2);
  EXPECT_EQ(t.Len(), 1u);
  EXPECT_EQ(t.Get("a"), nullptr);
  ASSERT_NE(t.Get("b"), nullptr);
  EXPECT_EQ(*t.Get("b"), 2);
}

TEST(HashTableTest, SampleKeysReturnsOnlyEntriesActuallyInTheTable) {
  HashTable<int> t;
  std::set<std::string> valid_keys;
  for (int i = 0; i < 40; ++i) {
    std::string k = "k" + std::to_string(i);
    t.Set(k, i);
    valid_keys.insert(k);
  }
  auto sample = t.SampleKeys(10);
  EXPECT_LE(sample.size(), 10u);
  for (const auto& entry : sample) {
    EXPECT_TRUE(valid_keys.count(entry.first)) << "sampled key not in table: " << entry.first;
    ASSERT_NE(entry.second, nullptr);
  }
}

TEST(HashTableTest, SampleKeysOnEmptyTableReturnsEmpty) {
  HashTable<int> t;
  EXPECT_TRUE(t.SampleKeys(5).empty());
}

TEST(HashTableTest, SurvivesHeavyDeletionWithoutLosingRemainingEntries) {
  // Indirectly exercises MaybeShrink (buckets_ itself is private, so not
  // directly observable) — the real motivation for this being correct at
  // all lives in Store's eviction/LRU behavior (SampleKeys' quality
  // depends on the table not staying oversized-and-sparse after heavy
  // deletion), but the basic invariant — surviving entries stay reachable
  // through a shrink — belongs here, at the data-structure level.
  HashTable<int> t;
  for (int i = 0; i < 1000; ++i) {
    t.Set("k" + std::to_string(i), i);
  }
  for (int i = 0; i < 990; ++i) {
    t.Delete("k" + std::to_string(i));
  }
  EXPECT_EQ(t.Len(), 10u);
  for (int i = 990; i < 1000; ++i) {
    const int* v = t.Get("k" + std::to_string(i));
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, i);
  }
}

TEST(HashTableTest, MoveConstructionTransfersOwnership) {
  HashTable<int> t;
  t.Set("a", 1);
  HashTable<int> moved(std::move(t));
  ASSERT_NE(moved.Get("a"), nullptr);
  EXPECT_EQ(*moved.Get("a"), 1);
  EXPECT_EQ(moved.Len(), 1u);
}

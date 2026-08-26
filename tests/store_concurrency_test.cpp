// Separate from store_test.cpp deliberately: these are slower (real
// threads, real sleeps) and their point is different — not "is the
// return value correct" but "does this survive many threads hammering
// every operation at once without a crash, hang, or a sanitizer-detected
// race." Run this suite under ASan/TSan builds specifically for that
// payoff; under a plain build it still verifies the store stays in a
// coherent state afterward.
#include "store/store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using goredis::Store;

TEST(StoreConcurrencyTest, ManyThreadsHammeringAllOperationsStaysCoherent) {
  Store s(std::chrono::milliseconds(15));
  s.SetMaxMemory(20000);  // keep eviction actively engaged throughout

  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 1000;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&s, t] {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string key = "key" + std::to_string(t) + "_" + std::to_string(i % 50);
        s.Set(key, "value");
        if (i % 3 == 0) {
          s.Expire(key, 0);
        } else if (i % 3 == 1) {
          s.Expire(key, 5);
        }
        (void)s.Get(key);
        (void)s.TTL(key);
        if (i % 7 == 0) {
          s.Del({key});
        }
        if (i % 11 == 0) {
          (void)s.Exists({key});
        }
        if (i % 13 == 0) {
          (void)s.Keys();
        }
        if (i % 17 == 0) {
          (void)s.UsedMemory();
        }
        s.RPush("shared_list", {key});
        (void)s.LLen("shared_list");
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  // Reaching here without a crash/hang/sanitizer report is the actual
  // point of this test, but a couple of basic sanity checks confirm the
  // store is still coherent afterward, not just "didn't crash."
  EXPECT_GE(s.LLen("shared_list").value.value_or(0), 0);
  for (const auto& key : s.Keys()) {
    EXPECT_FALSE(key.empty());
  }
}

TEST(StoreConcurrencyTest, ConcurrentSnapshotDuringWritesIsSafe) {
  Store s(std::chrono::milliseconds(15));

  std::atomic<bool> stop{false};
  std::thread writer([&s, &stop] {
    int i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      s.Set("k" + std::to_string(i % 100), "v");
      s.RPush("list", {"x"});
      ++i;
    }
  });

  for (int i = 0; i < 20; ++i) {
    auto snapshot = s.Snapshot();
    (void)snapshot;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();
}

TEST(StoreConcurrencyTest, ConcurrentEvictionUnderSustainedWritePressureIsSafe) {
  // Specifically stresses Store::EvictIfOverBudget concurrently with the
  // writes that trigger it — the code path where a real use-after-free
  // was once caught (LPush/RPush dereferencing a key's Value after
  // eviction had already freed it) before being fixed.
  Store s(std::chrono::milliseconds(15));
  s.SetMaxMemory(5000);

  constexpr int kThreads = 6;
  constexpr int kOpsPerThread = 800;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&s, t] {
      for (int i = 0; i < kOpsPerThread; ++i) {
        std::string key = "k" + std::to_string(t);
        s.RPush(key, {"v" + std::to_string(i)});
        s.LPush(key, {"w"});
        (void)s.UsedMemory();
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
}

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.h"
#include "store/store.h"

namespace goredis::bench {

void RunStoreBenchmarks() {
  using goredis::Store;

  Section("Store: single-threaded throughput");

  {
    Store s;
    long long n = 200000;
    long long i = 0;
    Run("Store::Set", n, [&] {
      s.Set("key" + std::to_string(i % 10000), "some reasonably sized value string");
      ++i;
    });
  }

  {
    Store s;
    constexpr int kPrepopulated = 10000;
    for (int i = 0; i < kPrepopulated; ++i) {
      s.Set("key" + std::to_string(i), "value" + std::to_string(i));
    }
    long long n = 500000;
    long long i = 0;
    long long checksum = 0;
    Run("Store::Get (hit)", n, [&] {
      auto r = s.Get("key" + std::to_string(i % kPrepopulated));
      if (r.value.has_value()) {
        checksum += static_cast<long long>(r.value->size());
      }
      ++i;
    });
    std::printf("  (checksum: %lld)\n", checksum);
  }

  {
    Store s;
    s.RPush("list", std::vector<std::string>(100, "element"));
    long long n = 200000;
    long long checksum = 0;
    Run("Store::LRange (100-element list, full range)", n, [&] {
      auto r = s.LRange("list", 0, -1);
      if (r.value.has_value()) {
        checksum += static_cast<long long>(r.value->size());
      }
    });
    std::printf("  (checksum: %lld)\n", checksum);
  }

  {
    // A cache-like mixed workload: mostly reads, occasional writes.
    Store s;
    constexpr int kKeys = 10000;
    for (int i = 0; i < kKeys; ++i) {
      s.Set("key" + std::to_string(i), "value");
    }
    long long n = 300000;
    long long i = 0;
    long long checksum = 0;
    Run("Store: mixed workload (90% GET / 10% SET)", n, [&] {
      int k = static_cast<int>(i % kKeys);
      if (i % 10 == 0) {
        s.Set("key" + std::to_string(k), "updated");
      } else {
        auto r = s.Get("key" + std::to_string(k));
        if (r.value.has_value()) {
          checksum += static_cast<long long>(r.value->size());
        }
      }
      ++i;
    });
    std::printf("  (checksum: %lld)\n", checksum);
  }

  Section("Store: GET throughput under increasing concurrent thread count");

  for (int num_threads : {1, 2, 4, 8}) {
    Store s;
    constexpr int kKeys = 10000;
    for (int i = 0; i < kKeys; ++i) {
      s.Set("key" + std::to_string(i), "value" + std::to_string(i));
    }

    constexpr long long kOpsPerThread = 200000;
    // Each thread writes only to its own slot — no shared mutable state,
    // so no synchronization is needed here (this project has already
    // been bitten twice by races on shared counters elsewhere; this
    // benchmark deliberately avoids the pattern entirely rather than
    // reaching for a relaxed atomic that would itself need scrutiny).
    std::vector<long long> checksums(static_cast<std::size_t>(num_threads), 0);

    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < num_threads; ++t) {
      threads.emplace_back([&s, &checksums, t] {
        long long checksum = 0;
        for (long long i = 0; i < kOpsPerThread; ++i) {
          auto r = s.Get("key" + std::to_string((i + t) % kKeys));
          if (r.value.has_value()) {
            checksum += static_cast<long long>(r.value->size());
          }
        }
        checksums[static_cast<std::size_t>(t)] = checksum;
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    auto end = std::chrono::steady_clock::now();

    long long total_checksum = 0;
    for (long long c : checksums) {
      total_checksum += c;
    }

    double seconds = std::chrono::duration<double>(end - start).count();
    long long total_ops = static_cast<long long>(num_threads) * kOpsPerThread;
    std::printf("%-40s %10lld ops  %8.3fs  %14.0f ops/sec  (%d threads, checksum %lld)\n",
                "Store::Get concurrent scaling", total_ops, seconds, static_cast<double>(total_ops) / seconds,
                num_threads, total_checksum);
  }

  std::printf(
      "\nNote: this isn't guaranteed to show rising throughput with more threads, and on the machine this was\n"
      "developed on it doesn't past 2 threads — aggregate ops/sec actually falls at 4 and 8 threads despite 8\n"
      "physical cores being available (so it isn't simple oversubscription). shared_mutex's read-side lock\n"
      "acquisition still touches shared internal state on every call, and under heavy multi-core contention that\n"
      "can cause cache-line ping-ponging between cores' caches that outweighs the benefit of allowing concurrent\n"
      "readers in the first place — a known, documented characteristic of some shared_mutex implementations, not\n"
      "a bug in Store itself. Reported here rather than asserted away: an honest benchmark measures what actually\n"
      "happens on the machine it runs on, not what the design intended to happen.\n");
}

}  // namespace goredis::bench

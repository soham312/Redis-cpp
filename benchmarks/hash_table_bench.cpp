#include <string>
#include <unordered_map>

#include "bench_util.h"
#include "store/hash_table.h"

namespace goredis::bench {

void RunHashTableBenchmarks() {
  using goredis::HashTable;

  Section("HashTable (this project's own, hand-rolled)");

  {
    HashTable<int> t;
    long long n = 500000;
    long long i = 0;
    Run("HashTable::Set (sequential insert, growing)", n, [&] {
      t.Set("key" + std::to_string(i), static_cast<int>(i));
      ++i;
    });
  }

  {
    HashTable<int> t;
    constexpr int kPrepopulated = 100000;
    for (int i = 0; i < kPrepopulated; ++i) {
      t.Set("key" + std::to_string(i), i);
    }

    long long n = 1000000;
    long long i = 0;
    long long checksum = 0;  // printed below so these lookups can't be optimized away as dead code
    Run("HashTable::Get (hit)", n, [&] {
      int* v = t.Get("key" + std::to_string(i % kPrepopulated));
      if (v != nullptr) {
        checksum += *v;
      }
      ++i;
    });
    std::printf("  (checksum: %lld)\n", checksum);

    i = 0;
    Run("HashTable::Get (miss)", n, [&] {
      int* v = t.Get("nokey" + std::to_string(i % kPrepopulated));
      if (v != nullptr) {
        checksum += *v;
      }
      ++i;
    });
  }

  Section("std::unordered_map (reference comparison only — never used in the actual store)");

  {
    std::unordered_map<std::string, int> m;
    long long n = 500000;
    long long i = 0;
    Run("unordered_map::operator[] (sequential insert, growing)", n, [&] {
      m["key" + std::to_string(i)] = static_cast<int>(i);
      ++i;
    });
  }

  {
    std::unordered_map<std::string, int> m;
    constexpr int kPrepopulated = 100000;
    for (int i = 0; i < kPrepopulated; ++i) {
      m["key" + std::to_string(i)] = i;
    }

    long long n = 1000000;
    long long i = 0;
    long long checksum = 0;
    Run("unordered_map::find (hit)", n, [&] {
      auto it = m.find("key" + std::to_string(i % kPrepopulated));
      if (it != m.end()) {
        checksum += it->second;
      }
      ++i;
    });
    std::printf("  (checksum: %lld)\n", checksum);
  }
}

}  // namespace goredis::bench

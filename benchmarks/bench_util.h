// A hand-rolled std::chrono timing loop, not a full benchmarking
// framework (no warm-up detection, outlier rejection, or statistical
// confidence intervals — see Google Benchmark for that level of rigor).
// Deliberately simple and dependency-free: enough to get real,
// presentable throughput/latency numbers without pulling in another
// external dependency just for this stage.
#pragma once

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

namespace goredis::bench {

inline void Run(const std::string& name, long long iterations, const std::function<void()>& fn) {
  auto start = std::chrono::steady_clock::now();
  for (long long i = 0; i < iterations; ++i) {
    fn();
  }
  auto end = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(end - start).count();
  double ops_per_sec = static_cast<double>(iterations) / seconds;
  double ns_per_op = seconds * 1e9 / static_cast<double>(iterations);
  std::printf("%-55s %10lld ops  %8.3fs  %14.0f ops/sec  %9.1f ns/op\n", name.c_str(), iterations, seconds,
              ops_per_sec, ns_per_op);
}

inline void Section(const std::string& title) { std::printf("\n-- %s --\n", title.c_str()); }

}  // namespace goredis::bench

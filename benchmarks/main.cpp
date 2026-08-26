#include <cstdio>

namespace goredis::bench {
void RunHashTableBenchmarks();
void RunStoreBenchmarks();
void RunServerBenchmarks();
}  // namespace goredis::bench

int main() {
  std::printf("goredis benchmarks\n");
  std::printf("===================\n");
  std::printf(
      "Note: this target forces -O2 regardless of the overall CMAKE_BUILD_TYPE (see "
      "benchmarks/CMakeLists.txt) — meaningful numbers require real optimization; an unoptimized build's "
      "timings would reflect missing inlining/vectorization, not the algorithms being measured.\n");

  goredis::bench::RunHashTableBenchmarks();
  goredis::bench::RunStoreBenchmarks();
  goredis::bench::RunServerBenchmarks();

  return 0;
}

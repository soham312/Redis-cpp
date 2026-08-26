cmake_minimum_required(VERSION 3.16)
project(goredis_cpp CXX)

# C++17 for std::shared_mutex (readers-writer lock), std::optional, and
# structured bindings. Nothing here needs anything newer.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emitted so editors/clangd can find the real compile flags (include paths,
# -std, etc.) instead of guessing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_subdirectory(src)

# Stage 6: unit tests, via Google Test (pulled in through CMake
# FetchContent — see tests/CMakeLists.txt for why a pinned version
# rather than a floating branch). Optional and default-ON: a consumer
# who only wants the library or the server binary can skip building the
# whole GTest dependency with -DGOREDIS_BUILD_TESTS=OFF.
option(GOREDIS_BUILD_TESTS "Build the GoogleTest-based unit test suite" ON)
if(GOREDIS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Stage 7: hand-rolled std::chrono benchmarks (no external dependency —
# see benchmarks/bench_util.h for why). Optional and default-ON, same
# reasoning as GOREDIS_BUILD_TESTS above.
option(GOREDIS_BUILD_BENCHMARKS "Build the benchmark suite" ON)
if(GOREDIS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

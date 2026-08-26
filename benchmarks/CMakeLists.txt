add_executable(goredis_benchmarks
  main.cpp
  hash_table_bench.cpp
  store_bench.cpp
  server_bench.cpp
)
target_include_directories(goredis_benchmarks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(goredis_benchmarks PRIVATE cxx_std_17)
# goredis_store isn't listed explicitly — goredis_server already
# PUBLIC-links it, so CMake pulls it in transitively.
target_link_libraries(goredis_benchmarks PRIVATE goredis_server)

# Benchmarks need real optimization to produce meaningful numbers,
# regardless of whatever CMAKE_BUILD_TYPE the rest of the project happens
# to be configured with (this project defaults to Debug — see the
# top-level CMakeLists.txt). -O2 is forced on this target specifically
# rather than requiring the user to remember to reconfigure the whole
# project as Release just to run benchmarks.
target_compile_options(goredis_benchmarks PRIVATE -O2)

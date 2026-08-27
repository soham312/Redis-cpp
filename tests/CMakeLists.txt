include(FetchContent)

# Pinned to a specific tagged release, not a floating branch: a build
# that passes today shouldn't silently start failing (or silently start
# testing different behavior) because upstream GoogleTest changed
# underneath this project later.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
# GoogleTest's own build instructions recommend this — it keeps GTest's
# runtime library choice consistent with the rest of the build rather
# than mismatched, which matters most on Windows but is harmless to set
# everywhere.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(goredis_tests
  hash_table_test.cpp
  store_test.cpp
  store_concurrency_test.cpp
  rdb_test.cpp
  aof_test.cpp
  resp_value_test.cpp
  command_parser_test.cpp
  dispatcher_test.cpp
  tcp_server_test.cpp
)
target_compile_features(goredis_tests PRIVATE cxx_std_17)
# goredis_store and goredis_resp aren't listed explicitly: both
# goredis_persistence and goredis_server already PUBLIC-link goredis_store,
# and goredis_server already PUBLIC-links goredis_resp, so CMake pulls
# both in transitively — listing them again here just produced a
# harmless but noisy "ignoring duplicate libraries" linker warning.
target_link_libraries(goredis_tests PRIVATE
  goredis_persistence
  goredis_server
  GTest::gtest_main
)

# Registers each TEST()/TEST_F() as its own CTest entry (by running the
# binary once with --gtest_list_tests to discover them), rather than one
# monolithic "did the whole binary exit 0" test — so `ctest` output and
# `ctest -R <name>` both work at the level of individual test cases.
gtest_discover_tests(goredis_tests)

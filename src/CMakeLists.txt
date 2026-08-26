# The storage engine as a static library — kept separate from any
# executable (the server/client binaries land here in a later stage) so
# tests can link against it directly without pulling in networking code.
add_library(goredis_store STATIC
  store/store.cpp
)

target_include_directories(goredis_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(goredis_store PUBLIC cxx_std_17)

# Stage 2 (TTL/expiry) added a background sweeper thread (std::thread,
# std::mutex, std::condition_variable). macOS/clang links pthread
# implicitly, but Linux/gcc does not — Threads::Threads is the portable
# way to pull in whatever the platform needs for both.
find_package(Threads REQUIRED)
target_link_libraries(goredis_store PUBLIC Threads::Threads)

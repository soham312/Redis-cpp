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

# Stage 4 (persistence): RDB-style snapshot save/load, kept as its own
# library — separate from goredis_store — so the storage engine itself
# stays free of file I/O concerns, and so a future consumer that only
# needs the in-memory store (e.g. a unit test) doesn't have to pull in
# persistence code it never calls.
add_library(goredis_persistence STATIC
  persistence/rdb.cpp
)
target_include_directories(goredis_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(goredis_persistence PUBLIC cxx_std_17)
target_link_libraries(goredis_persistence PUBLIC goredis_store)

# Stage 5 (networking): RESP2 protocol encode/decode, kept independent of
# both Store and sockets — it only knows about bytes and strings, so it's
# reusable (and independently testable) without pulling in either.
add_library(goredis_resp STATIC
  resp/resp_value.cpp
  resp/command_parser.cpp
)
target_include_directories(goredis_resp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(goredis_resp PUBLIC cxx_std_17)

# Stage 5 (networking): command dispatch (RESP <-> Store) and the raw
# POSIX-socket TCP server, thread-per-connection.
add_library(goredis_server STATIC
  server/dispatcher.cpp
  server/tcp_server.cpp
)
target_include_directories(goredis_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(goredis_server PUBLIC cxx_std_17)
target_link_libraries(goredis_server PUBLIC goredis_store goredis_resp Threads::Threads)

# The actual server executable, linking everything above together.
# goredis_store isn't listed explicitly — both goredis_persistence and
# goredis_server already PUBLIC-link it, so CMake pulls it in
# transitively; listing it again here just produced a harmless but noisy
# "ignoring duplicate libraries" linker warning.
add_executable(goredis-server
  main.cpp
)
target_link_libraries(goredis-server PRIVATE goredis_persistence goredis_server)

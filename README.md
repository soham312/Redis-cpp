# goredis-cpp

A Redis-like in-memory key-value store, built from scratch in modern C++17 — hand-rolled hash table, TTL/expiry, approximate-LRU eviction, RDB-style persistence, and a raw-POSIX-socket TCP server speaking RESP2, all with no external dependencies in the production code path.

This is a from-scratch systems project, not a wrapper around existing libraries: **no `std::unordered_map`, no networking framework, no serialization library.** The one external dependency in the whole repo is GoogleTest, and it's used only by the test suite — never linked into the server binary.

## Why this exists

Built as a portfolio project to demonstrate manual memory management (RAII, no raw `new`/`delete`), concurrent systems design (`std::shared_mutex`, a background sweeper thread, thread-per-connection networking), and the kind of low-level protocol/data-structure work that's usually hidden behind a library. Every non-obvious design decision is explained in the code itself — the comments are written to be defensible in an interview, not just descriptive.

## Status

All 8 planned stages are implemented:

| Stage | What it added |
|---|---|
| 1. Core store | Hand-rolled hash table (FNV-1a, chaining, dynamic resize/shrink), thread-safe `Store` API over it |
| 2. TTL / expiry | `EXPIRE`/`TTL`, lazy expiration on read, active background sweep |
| 3. Eviction | Memory-budgeted approximate LRU (Redis-style random sampling, not an exact list) |
| 4. Persistence | RDB-style binary snapshot save/load, with correct downtime-expiry semantics |
| 5. Networking | Raw POSIX sockets, thread-per-connection, hand-rolled RESP2 parser/encoder |
| 6. Tests | 110 GoogleTest cases covering the hash table, store, persistence, protocol, and server, run clean under ASan/UBSan and ThreadSanitizer |
| 7. Benchmarks | Hand-rolled throughput/latency benchmarks for the hash table, store, and full network round trip |
| 8. Docs | This file |

## Quick start

```sh
# Configure and build everything (library, server, tests, benchmarks)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run the server (Ctrl+C for a graceful shutdown + final save)
./build/src/goredis-server --port 6380 --rdb dump.grdb

# Talk to it with anything that speaks RESP, or just netcat for a quick check
# (the server also accepts plain-text inline commands for exactly this reason):
printf 'PING\r\nSET foo bar\r\nGET foo\r\n' | nc -w1 127.0.0.1 6380
```

Requires CMake 3.16+ and a C++17 compiler. Tested with AppleClang on macOS; POSIX sockets and pthreads mean it should build unmodified on Linux.

### CLI options

| Flag | Default | Meaning |
|---|---|---|
| `--port N` | `6380` | TCP port to listen on (not Redis's default `6379`, to avoid colliding with a real `redis-server` during local testing) |
| `--rdb PATH` | `dump.grdb` | Snapshot file: loaded on startup if present, saved on graceful shutdown |
| `--maxmemory N` | `0` (unlimited) | Approximate memory budget in bytes; triggers eviction once exceeded |

### Build options

| CMake option | Default | Effect |
|---|---|---|
| `GOREDIS_BUILD_TESTS` | `ON` | Build the GoogleTest suite (fetched via `FetchContent`) |
| `GOREDIS_BUILD_BENCHMARKS` | `ON` | Build the benchmark suite |

Turn either off (`-DGOREDIS_BUILD_TESTS=OFF`) to skip building it — useful if you only want the library or server binary and don't want to pull in GoogleTest at all.

## Architecture

```
src/
  store/            Stages 1–3: the storage engine
    hash_table.h       hand-rolled hash table (templated on value type, string keys)
    value.h            tagged union of string/list, TTL, and LRU-recency metadata
    store.h/.cpp        thread-safe API over the hash table (Set/Get/Del/Expire/LPush/…)
  persistence/       Stage 4: RDB-style snapshotting
    rdb.h/.cpp          SaveRdb/LoadRdb — binary format, checksummed, atomic write-then-rename
  resp/              Stage 5: the wire protocol
    resp_value.h/.cpp   RESP2 reply encoding
    command_parser.h/.cpp  incremental request parsing (multibulk + inline)
  server/            Stage 5: networking
    dispatcher.h/.cpp   RESP command -> Store call -> RESP reply
    tcp_server.h/.cpp   POSIX sockets, thread-per-connection
  main.cpp           the server executable: wires the above together

tests/               Stage 6: GoogleTest suite (one file per module above)
benchmarks/          Stage 7: hand-rolled throughput/latency benchmarks
```

Each layer only depends on the ones below it, and each is its own CMake static library (`goredis_store`, `goredis_persistence`, `goredis_resp`, `goredis_server`) — `Store` has zero knowledge of file I/O or sockets, and the RESP layer has zero knowledge of `Store`. The command dispatcher is the only place that bridges "parsed request" to "store operation."

### A request's path through the system

```
client                      recv() bytes
  │                              │
  ▼                              ▼
TCP socket ──▶ TcpServer::HandleClient ──▶ CommandParser (incremental, handles
                     │                      partial/pipelined TCP reads)
                     ▼
              Dispatch(Store&, args) ──▶ Store::{Get,Set,Expire,LPush,...}
                     │                        │
                     ▼                        ▼
              RespValue reply          std::shared_mutex-guarded HashTable<Value>
                     │
                     ▼
              send() bytes back to the client
```

## Design decisions worth knowing about

These are the choices most likely to come up if you're reading this as a portfolio piece — each is explained at length in the relevant source file's comments; this is the short version.

- **Custom hash table, not `std::unordered_map`.** Separate chaining, FNV-1a hashing (explicit and explainable rather than opaque — noted in the code as not cryptographically secure, a deliberate tradeoff), power-of-two capacity with a bitmask instead of modulo, amortized-O(1) resize on growth **and** shrink-on-delete (added in Stage 3 once eviction made delete-heavy workloads common — a table that only grows would leave `SampleKeys` sampling a mostly-empty array after heavy eviction).

- **`std::shared_mutex` over `Store`, chosen for concurrent reads — with a benchmarked caveat.** Every read path (`Get`, `LLen`, `LRange`, …) takes a shared lock so multiple readers don't serialize behind each other. The Stage 7 benchmarks measured this directly and found throughput actually *drops* past 2 concurrent threads on the development machine (8 cores, so not oversubscription) — a documented characteristic of some `shared_mutex` implementations (read-lock acquisition still touches shared internal state, causing cache-line contention under load) rather than a flaw in the design's reasoning. Reported honestly in the benchmark output rather than glossed over.

- **Approximate LRU, not an exact linked list.** An exact LRU cache (hash map + intrusive doubly-linked list, the classic interview data structure) would require every read to take the *exclusive* lock to safely update the list — which would serialize all reads and defeat the entire point of `shared_mutex`. Instead, each entry carries an atomic **logical clock** stamp (not a wall-clock timestamp — an earlier version used milliseconds and a burst of same-millisecond operations left ties with no real signal; a monotonic counter can't tie), and eviction samples a handful of random keys and evicts whichever looks oldest — the same trade-off real Redis's own `maxmemory-samples` makes, and for the same reason.

- **TTLs stored as `steady_clock`, converted to `system_clock` only for persistence.** Internally, monotonic time is immune to wall-clock adjustments. But a `steady_clock::time_point` is meaningless across a process restart, so the RDB layer converts to an absolute wall-clock timestamp at save time — and critically, checks it against wall-clock time again *at load time*, so a key that should have expired while the process was down doesn't get a fresh countdown.

- **RDB over AOF for persistence.** AOF logs individual commands for replay, but there was no command parser/dispatcher yet at that point in the build order (that's Stage 5). RDB instead snapshots `Store`'s data directly via one atomic locked pass, fitting cleanly before a command layer exists to produce a log.

- **Thread-per-connection networking, not epoll/kqueue.** An event loop would scale further with less per-connection overhead, but `epoll` is Linux-only and `kqueue` is BSD/macOS-only — supporting both means two implementations behind an abstraction, which is real complexity this project's scale doesn't call for. Shutdown correctness got real attention despite the simple model: `shutdown()`, not `close()`, is used to unblock a client thread's `recv()` from another thread (the textbook-correct, safe way to do it); the SIGINT/SIGTERM handler touches only a `sig_atomic_t` flag (the one thing the C++ standard guarantees is safe inside a signal handler) rather than calling anything mutex-based directly.

## Supported commands

`PING`, `ECHO`, `SET` (plus `SET key value EX seconds`), `GET`, `DEL`, `EXISTS`, `KEYS *` (only the match-all pattern — no glob matching), `FLUSHALL`, `EXPIRE`, `TTL`, `LPUSH`, `RPUSH`, `LLEN`, `LRANGE`.

Every error path (unknown command, wrong arity, `WRONGTYPE`, non-integer argument) returns a RESP error reply with wording matching real Redis's own error messages where applicable.

## Testing

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure -j
```

110 tests across the hash table, store (including TTL/eviction/persistence), RESP protocol, and a real socket-based end-to-end suite for the server. The concurrency-sensitive tests are meant to be run under sanitizers during development:

```sh
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
                          -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j
ctest --test-dir build-tsan -j
```

(swap `thread` for `address,undefined` for ASan/UBSan). Two real, subtle concurrency bugs were caught this way during development — a use-after-free in the eviction path and a missing release/acquire pairing on a thread-completion counter — both documented at the fix site in the code.

## Benchmarking

```sh
cmake --build build -j
./build/benchmarks/goredis_benchmarks
```

Hand-rolled `std::chrono` timing (not Google Benchmark — avoids a second slow `FetchContent` dependency for this stage). Covers hash table and `Store` throughput, a reference comparison against `std::unordered_map`, concurrent-read scaling, and full end-to-end network round-trip latency. The binary forces `-O2` on itself regardless of the overall build type, since this project defaults to `CMAKE_BUILD_TYPE=Debug`.

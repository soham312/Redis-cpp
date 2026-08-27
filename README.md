# goredis-cpp

[![CI](https://github.com/soham312/Redis-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/soham312/Redis-cpp/actions/workflows/ci.yml)

A Redis-like in-memory key-value store, built from scratch in modern C++17 — hand-rolled hash table, TTL/expiry, approximate-LRU eviction, string/list/hash data types, dual RDB-and-AOF persistence, and a raw-POSIX-socket TCP server speaking RESP2, all with no external dependencies in the production code path.

This is a from-scratch systems project, not a wrapper around existing libraries: **no `std::unordered_map`, no networking framework, no serialization library.** The one external dependency in the whole repo is GoogleTest, and it's used only by the test suite — never linked into the server binary.

## Why this exists

Built as a portfolio project to demonstrate manual memory management (RAII, no raw `new`/`delete`), concurrent systems design (`std::shared_mutex`, a background sweeper thread, thread-per-connection networking), and the kind of low-level protocol/data-structure work that's usually hidden behind a library. Every non-obvious design decision is explained in the code itself — the comments are written to be defensible in an interview, not just descriptive.

## Status

All 8 originally planned stages are implemented, plus three follow-on additions that reuse the same architecture rather than bolt on something new:

| Stage | What it added |
|---|---|
| 1. Core store | Hand-rolled hash table (FNV-1a, chaining, dynamic resize/shrink), thread-safe `Store` API over it |
| 2. TTL / expiry | `EXPIRE`/`TTL`, lazy expiration on read, active background sweep |
| 3. Eviction | Memory-budgeted approximate LRU (Redis-style random sampling, not an exact list) |
| 4. Persistence | RDB-style binary snapshot save/load, with correct downtime-expiry semantics |
| 5. Networking | Raw POSIX sockets, thread-per-connection, hand-rolled RESP2 parser/encoder |
| 6. Tests | GoogleTest suite covering the hash table, store, persistence, protocol, and server, run clean under ASan/UBSan and ThreadSanitizer |
| 7. Benchmarks | Hand-rolled throughput/latency benchmarks for the hash table, store, and full network round trip |
| 8. Docs | This file |
| 9. Hash type | `HSET`/`HGET`/`HDEL`/`HGETALL`/`HLEN`/`HEXISTS`/`HKEYS`/`HVALS` — field storage reuses the project's own `HashTable<V>` rather than reaching for `std::unordered_map` |
| 10. AOF persistence | A second, complementary persistence mode (`--aof`): every write command is logged and fsynced as it happens, replayed on startup — see [Persistence: RDB vs. AOF](#persistence-rdb-vs-aof) below |
| 11. CI / Docker | GitHub Actions (build+test matrix across Linux/macOS, plus a dedicated ASan/UBSan/TSan job) and a multi-stage `Dockerfile` |

`KEYS` also gained real glob matching (`*`, `?`) as part of stage 9's cleanup — see [Supported commands](#supported-commands).

The test suite has grown alongside these: 142 GoogleTest cases as of stage 11, still run clean under ASan/UBSan and ThreadSanitizer.

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

### Running with Docker

```sh
docker build -t goredis-cpp .
docker run --rm -p 6380:6380 -v goredis-data:/data goredis-cpp
```

The image is a multi-stage build (compiler toolchain in the build stage only; the runtime image ships just the `goredis-server` binary) and defaults to RDB persistence at `/data/dump.grdb` — mount a named volume there to persist across container restarts.

### CLI options

| Flag | Default | Meaning |
|---|---|---|
| `--port N` | `6380` | TCP port to listen on (not Redis's default `6379`, to avoid colliding with a real `redis-server` during local testing) |
| `--rdb PATH` | `dump.grdb` | Snapshot file: loaded on startup if present, saved on graceful shutdown. Ignored if `--aof` is given. |
| `--aof PATH` | *(disabled)* | Enables AOF persistence instead of RDB: replays PATH on startup if present, appends+fsyncs every write command to it as it happens. See [Persistence: RDB vs. AOF](#persistence-rdb-vs-aof). |
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
  store/            Stages 1–3, 9: the storage engine
    hash_table.h       hand-rolled hash table (templated on value type, string keys)
    value.h            tagged union of string/list/hash, TTL, and LRU-recency metadata
    store.h/.cpp        thread-safe API over the hash table (Set/Get/Del/Expire/LPush/HSet/…)
  persistence/       Stage 4: RDB-style snapshotting
    rdb.h/.cpp          SaveRdb/LoadRdb — binary format, checksummed, atomic write-then-rename
  resp/              Stage 5: the wire protocol
    resp_value.h/.cpp   RESP2 reply encoding
    command_parser.h/.cpp  incremental request parsing (multibulk + inline)
  server/            Stage 5, 10: networking + command-log persistence
    dispatcher.h/.cpp   RESP command -> Store call -> RESP reply
    tcp_server.h/.cpp   POSIX sockets, thread-per-connection
    aof.h/.cpp          Stage 10: AofWriter (append+fsync) / LoadAof (replay via Dispatch)
  main.cpp           the server executable: wires the above together

tests/               Stage 6: GoogleTest suite (one file per module above)
benchmarks/          Stage 7: hand-rolled throughput/latency benchmarks
```

Each layer only depends on the ones below it, and each is its own CMake static library (`goredis_store`, `goredis_persistence`, `goredis_resp`, `goredis_server`) — `Store` has zero knowledge of file I/O or sockets, and the RESP layer has zero knowledge of `Store`. The command dispatcher is the only place that bridges "parsed request" to "store operation."

`aof.cpp` lives in `goredis_server`, not `goredis_persistence`, despite being persistence code: replaying an AOF log means re-running each command through `Dispatch()`, so it depends on the command layer — putting it under "persistence" would mean persistence depending on server, inverting the layering above. See `server/aof.h`'s header comment for the full reasoning.

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

- **RDB first, AOF later, and still mutually exclusive today.** AOF logs individual commands for replay, but there was no command parser/dispatcher yet at the point in the build order where persistence was first added (that's Stage 5) — RDB instead snapshotted `Store`'s data directly via one atomic locked pass. AOF was added in Stage 10 once the command layer existed to produce a log from, but the server still runs in exactly one mode at a time (`--rdb` or `--aof`), not both simultaneously — see [Persistence: RDB vs. AOF](#persistence-rdb-vs-aof) for why running both isn't just "free" once each already works alone.

- **Hash fields stored in the project's own `HashTable<V>`, not `std::unordered_map`.** Redis's own Hash type is, structurally, exactly the "string key → string value" problem this project's hand-rolled table already solves — so `Value::hash_value` is a `HashTable<std::string>`, reusing the same resize/shrink/chaining logic every other key in the store already relies on, rather than introducing the one standard container this whole project deliberately avoids.

- **`AofWriter::Append` fsyncs on every call, and is mutex-guarded.** `appendfsync always` (Redis's own terminology) is the simplest fully-durable policy — every `Append()` that returns true is guaranteed on disk, no batching window to reason about — at the cost of a syscall-and-wait per write command; real Redis defaults to batching once a second (`everysec`) for throughput this project's scale doesn't need. The mutex matters because one `AofWriter` is shared across every client-handler thread (this server is thread-per-connection — see below): without it, two threads' `write()` calls could interleave mid-command and corrupt the log, since a single `Append()` can span more than one `write()` syscall on a partial write. Caught and fixed via a dedicated `AofTest.ConcurrentAppendsDoNotCorruptTheLog` test run under ThreadSanitizer, the same discipline the other concurrency bugs mentioned in Testing below were caught with.

- **Thread-per-connection networking, not epoll/kqueue.** An event loop would scale further with less per-connection overhead, but `epoll` is Linux-only and `kqueue` is BSD/macOS-only — supporting both means two implementations behind an abstraction, which is real complexity this project's scale doesn't call for. Shutdown correctness got real attention despite the simple model: `shutdown()`, not `close()`, is used to unblock a client thread's `recv()` from another thread (the textbook-correct, safe way to do it); the SIGINT/SIGTERM handler touches only a `sig_atomic_t` flag (the one thing the C++ standard guarantees is safe inside a signal handler) rather than calling anything mutex-based directly.

## Persistence: RDB vs. AOF

The server runs with exactly one persistence mode active, chosen by which flag you pass — `--rdb PATH` (the default) or `--aof PATH`:

| | RDB (`--rdb`) | AOF (`--aof`) |
|---|---|---|
| What's stored | A compact, point-in-time snapshot of the whole dataset | Every write command, in order, as it happened |
| Durability | Only as fresh as the last save (SIGINT/SIGTERM triggers one; nothing else does) | At most the last `fsync`'s worth of writes at risk — i.e. essentially none, since every `Append()` fsyncs before returning |
| Startup cost | Load one file | Replay every logged command from the start |
| File growth | Bounded (one snapshot, rewritten each save) | Unbounded — no AOF-rewrite/compaction is implemented (a real Redis periodically compacts its AOF down to snapshot-equivalent size; a natural, separate addition this project doesn't currently need) |
| TTL precision across a restart | Exact — anchored to absolute wall-clock time at save time (see `rdb.cpp`) | Approximate for `EXPIRE`/`SET ... EX` specifically: replay re-arms the TTL relative to *replay* time, not the original command's time, since this project has no `PEXPIREAT` to rewrite a relative expiry into an absolute one the way real Redis's own AOF does before logging it (see the comment on `EXPIRE` in `dispatcher.cpp`) |

Why not run both at once, the way a real Redis deployment often does (AOF as the primary durability mechanism, RDB snapshots for point-in-time backups)? Because that requires deciding which one is authoritative when they've diverged after a crash — real, non-trivial complexity a portfolio-scale server doesn't need to take on. One mode, chosen up front, keeps the answer to "what does this server do on startup" a single unconditional branch in `main.cpp` rather than a reconciliation policy.

## Supported commands

`PING`, `ECHO`, `SET` (plus `SET key value EX seconds`), `GET`, `DEL`, `EXISTS`, `KEYS pattern` (`*` and `?` glob wildcards — no character classes), `FLUSHALL`, `EXPIRE`, `TTL`, `LPUSH`, `RPUSH`, `LLEN`, `LRANGE`, `HSET`, `HGET`, `HDEL`, `HGETALL`, `HLEN`, `HEXISTS`, `HKEYS`, `HVALS`.

Every error path (unknown command, wrong arity, `WRONGTYPE`, non-integer argument) returns a RESP error reply with wording matching real Redis's own error messages where applicable.

## Testing

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure -j
```

142 tests across the hash table, store (including TTL/eviction/persistence/hashes), RESP protocol, AOF logging/replay, and a real socket-based end-to-end suite for the server. The concurrency-sensitive tests are meant to be run under sanitizers during development:

```sh
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
                          -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j
ctest --test-dir build-tsan -j
```

(swap `thread` for `address,undefined` for ASan/UBSan). Three real, subtle concurrency bugs were caught this way during development — a use-after-free in the eviction path, a missing release/acquire pairing on a thread-completion counter, and (Stage 10) a missing lock around `AofWriter::Append` that let concurrent client threads interleave `write()` calls and corrupt the log — all documented at their fix site in the code.

CI (`.github/workflows/ci.yml`) runs the plain build+test matrix on every push/PR across Linux and macOS, plus a dedicated job that runs the same sanitizer builds described above — so a regression they'd catch can no longer merge unnoticed, rather than depending on someone remembering to run them locally.

## Benchmarking

```sh
cmake --build build -j
./build/benchmarks/goredis_benchmarks
```

Hand-rolled `std::chrono` timing (not Google Benchmark — avoids a second slow `FetchContent` dependency for this stage). Covers hash table and `Store` throughput, a reference comparison against `std::unordered_map`, concurrent-read scaling, and full end-to-end network round-trip latency. The binary forces `-O2` on itself regardless of the overall build type, since this project defaults to `CMAKE_BUILD_TYPE=Debug`.

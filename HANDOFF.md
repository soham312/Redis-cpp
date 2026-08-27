# Handoff — goredis-cpp

_Last updated: 2026-08-28_

## What this project is

A Redis-like in-memory key-value store built from scratch in modern C++17. It's a
from-scratch systems project, not a wrapper around existing libraries: no
`std::unordered_map`, no networking framework, no serialization library. The only
external dependency in the whole repo is GoogleTest, used solely by the test suite
(never linked into the server binary).

Built as a portfolio piece to demonstrate manual memory management (RAII, no raw
`new`/`delete`), concurrent systems design (`std::shared_mutex`, a background sweeper
thread, thread-per-connection networking), and low-level protocol/data-structure work
that's normally hidden behind a library. Comments throughout the code are written to
be defensible in an interview, not just descriptive — this repo is meant to be read.

It appears to be a C++ port of an earlier Go implementation ("goredis") — several
comments reference mirroring that original design.

## Status: original 8 stages complete, plus 3 follow-on stages added this session

| Stage | What it added | Where |
|---|---|---|
| 1 | Hand-rolled hash table (FNV-1a, separate chaining, power-of-two capacity, amortized-O(1) resize) + thread-safe `Store` | `src/store/hash_table.h`, `src/store/store.{h,cpp}` |
| 2 | TTL/expiry — `EXPIRE`/`TTL`, lazy expiration on read, active background sweep thread | `src/store/store.{h,cpp}` |
| 3 | Approximate-LRU eviction under a memory budget (Redis-style random sampling, plus hash-table shrink-on-delete to keep sampling quality up) | `src/store/store.cpp`, `src/store/hash_table.h` |
| 4 | RDB-style binary snapshot persistence (checksummed, atomic write-then-rename, correct downtime-expiry semantics) | `src/persistence/rdb.{h,cpp}` |
| 5 | Raw POSIX sockets, thread-per-connection TCP server, hand-rolled RESP2 parser/encoder | `src/server/`, `src/resp/` |
| 6 | GoogleTest suite across all modules, clean under ASan/UBSan and TSan | `tests/` |
| 7 | Hand-rolled `std::chrono` throughput/latency benchmarks (no Google Benchmark dependency) | `benchmarks/` |
| 8 | README polish / documentation pass | `README.md` |
| 9 | **Hash data type** (`HSET`/`HGET`/`HDEL`/`HGETALL`/`HLEN`/`HEXISTS`/`HKEYS`/`HVALS`), reusing the project's own `HashTable<V>` for field storage. Also added real glob matching (`*`/`?`) to `KEYS`, replacing the old match-all-only restriction. | `src/store/value.h`, `src/store/store.{h,cpp}`, `src/persistence/rdb.cpp` (new `kTypeHash` RDB tag, format version bumped 1→2), `src/server/dispatcher.cpp` (`GlobMatch`) |
| 10 | **AOF persistence** (`--aof PATH`, mutually exclusive with `--rdb`): every successful write command is RESP-encoded, appended, and fsynced; replayed via `Dispatch()` on startup. `AofWriter::Append` is mutex-guarded — a real, caught-under-TSan bug: one `AofWriter` is shared across every client thread (thread-per-connection), and an unguarded multi-syscall `write()` could interleave and corrupt the log. | `src/server/aof.{h,cpp}` (deliberately in `server/`, not `persistence/` — replay depends on `Dispatch()`, so putting it in `persistence/` would invert the project's layering), `src/main.cpp`, `src/server/dispatcher.{h,cpp}` (`AofWriter*` param), `src/server/tcp_server.{h,cpp}` |
| 11 | CI (`.github/workflows/ci.yml`: build+test matrix on Linux/macOS, plus a dedicated ASan/UBSan/TSan job) and a multi-stage `Dockerfile` | `.github/workflows/ci.yml`, `Dockerfile` |

Test count grew from 110 → **142**, all passing under plain, ASan/UBSan, and TSan
builds (verified this session, including two full repeat runs to rule out
test-file-hygiene flakiness — see the AOF section below).

**Not yet committed** — everything in stages 9–11 above, plus the original
`.gitignore` diff (a `*.grdb` entry) from before this session, are sitting as
uncommitted working-tree changes. Nothing has been pushed. Review and commit is the
natural next step once you're happy with it.

## Architecture

```
src/
  store/        storage engine — hash table, Value (tagged string/list/hash + TTL +
                LRU stamp), thread-safe Store API (Set/Get/Del/Expire/LPush/HSet/…)
  persistence/  RDB save/load — binary format, checksummed, atomic write-then-rename
  resp/         RESP2 reply encoding + incremental request parser (multibulk + inline)
  server/       dispatcher (RESP command -> Store call -> RESP reply), TcpServer
                (POSIX sockets, thread-per-connection), aof.{h,cpp} (AofWriter/LoadAof)
  main.cpp      wires it all together into the server executable

tests/          one GoogleTest file per module above (now includes aof_test.cpp)
benchmarks/     hash table / store / network-round-trip benchmarks
```

Strict layering: each module is its own CMake static library
(`goredis_store`, `goredis_persistence`, `goredis_resp`, `goredis_server`), and each
layer only depends on the ones below it. `Store` has zero knowledge of file I/O or
sockets; the RESP layer has zero knowledge of `Store`. The dispatcher is the only
place that bridges "parsed request" to "store operation." AOF's replay path needs
`Dispatch()`, so it lives inside `goredis_server` rather than `goredis_persistence`
to keep persistence from depending on the command layer.

Request path (unchanged): `TCP socket → TcpServer::HandleClient → CommandParser
(incremental, handles partial/pipelined reads) → Dispatch(Store&, args, AofWriter*)
→ Store::{Get,Set,...} → RespValue reply → send()`. The `AofWriter*` param is new
this session (defaults to `nullptr`, so every pre-existing call site still compiles
unchanged) — Dispatch logs a command to it right after a write actually succeeds,
the one place that already knows success/failure without re-deriving it.

## Key design decisions (the ones worth knowing before touching the code)

- **Custom hash table**: FNV-1a hashing (explicit, not cryptographically secure —
  a deliberate, documented tradeoff), power-of-two capacity + bitmask indexing,
  grows on load factor and **shrinks on delete** (added in Stage 3 so eviction-heavy
  workloads don't leave `SampleKeys` sampling a mostly-empty table).
- **`std::shared_mutex` over `Store`**: chosen for concurrent reads, but Stage 7's
  own benchmarks found throughput actually *drops* past 2 concurrent threads on the
  dev machine — a known `shared_mutex` characteristic (cache-line contention on
  read-lock acquisition), reported honestly rather than hidden.
- **Approximate LRU, not an exact list**: exact LRU needs an exclusive lock on every
  read to update a linked list, defeating the point of `shared_mutex`. Instead each
  `Value` carries an atomic **logical clock** stamp (not wall-clock — an earlier
  version used milliseconds and same-millisecond bursts produced ties with no
  signal), and eviction samples a handful of random keys via
  `HashTable::SampleKeys` and evicts the oldest-looking one — same tradeoff as
  Redis's own `maxmemory-samples`.
- **TTLs stored as `steady_clock`** internally (immune to wall-clock adjustments),
  **converted to `system_clock` only for RDB persistence**, and re-checked against
  wall-clock time again at load — so a key that should have expired while the
  process was down doesn't get a fresh countdown. **AOF's TTL replay is *not* this
  precise** — see below.
- **Hash type reuses `HashTable<std::string>`** for field storage rather than
  `std::unordered_map` — the Hash type is structurally the same "string key →
  string value" problem the project's own table already solves, so it gets the
  hand-rolled implementation for free, consistent with the project's "no
  `std::unordered_map`" rule everywhere else too.
- **RDB and AOF are mutually exclusive**, chosen by `--rdb` vs. `--aof` at startup —
  not run simultaneously the way a real Redis deployment often does. Combining them
  would mean deciding which is authoritative after a crash where they've diverged;
  that reconciliation-policy complexity was deliberately not taken on.
- **AOF's `EXPIRE`/`SET ... EX` replay is approximate, not exact**: it re-arms a
  key's TTL relative to *replay* time, not the original command time, because this
  project has no `PEXPIREAT` to rewrite a relative expiry into an absolute one
  before logging (real Redis's own AOF does exactly that rewrite). Documented
  in-code at the `EXPIRE` branch of `dispatcher.cpp` and in the README's
  "Persistence: RDB vs. AOF" table — an honestly-named tradeoff, not a bug.
- **`AofWriter::Append` fsyncs every call** (Redis's `appendfsync always`, not the
  faster but batched `everysec` default) — simplest fully-durable policy, no
  background flush thread needed, matching the project's habit of picking the
  easier-to-reason-about option when the extra throughput isn't needed at this
  scale.
- **Thread-per-connection**, not epoll/kqueue — avoids maintaining two
  platform-specific event-loop implementations for a project at this scale. Shutdown
  uses `shutdown()` (not `close()`) to safely unblock a client thread's `recv()`
  from another thread; the SIGINT/SIGTERM handler only touches a `sig_atomic_t` flag,
  with the actual shutdown work done from ordinary thread context in `main.cpp`'s
  poll loop.
- Three real concurrency bugs were caught during development via sanitizers: two
  pre-existing (a use-after-free in the eviction path, a missing release/acquire
  pairing on a thread-completion counter — documented at their fix sites), plus one
  caught **this session**: `AofWriter::Append` had no lock, and since one
  `AofWriter` is shared across every client-handler thread, concurrent `write()`
  calls could interleave mid-command and corrupt the log. Fixed with a
  `std::mutex`; verified via a dedicated `AofTest.ConcurrentAppendsDoNotCorruptTheLog`
  test that passes clean under TSan.

## Supported commands

`PING`, `ECHO`, `SET` (with `EX seconds`), `GET`, `DEL`, `EXISTS`, `KEYS pattern`
(now real `*`/`?` glob matching — no character classes), `FLUSHALL`, `EXPIRE`, `TTL`,
`LPUSH`, `RPUSH`, `LLEN`, `LRANGE`, `HSET`, `HGET`, `HDEL`, `HGETALL`, `HLEN`,
`HEXISTS`, `HKEYS`, `HVALS`. Error replies mirror real Redis wording where
applicable (unknown command, wrong arity, `WRONGTYPE`, non-integer argument).

## Build / run / test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/src/goredis-server --port 6380 --rdb dump.grdb    # or --aof dump.aof; --maxmemory N for eviction

ctest --test-dir build --output-on-failure -j   # 142 tests
./build/benchmarks/goredis_benchmarks
```

CMake options `GOREDIS_BUILD_TESTS` / `GOREDIS_BUILD_BENCHMARKS` (both default ON)
can be turned off to skip GTest/benchmark builds.

Docker: `docker build -t goredis-cpp .` then `docker run --rm -p 6380:6380 -v
goredis-data:/data goredis-cpp` (multi-stage build, RDB persistence to `/data` by
default). Verified the multi-stage build's *logic* by inspection — **not actually
built this session**, since no Docker daemon was running in this environment. Worth
a real `docker build` before treating it as verified.

Full sanitizer verification this session:
```sh
# ASan/UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DGOREDIS_BUILD_BENCHMARKS=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
# TSan: swap the sanitizer name to "thread" in both flags above
```
Both passed 142/142 clean.

## What's *not* there (natural next steps, not started)

- No AOF rewrite/compaction — the AOF file grows unbounded (documented, matches
  real Redis's own pre-rewrite behavior).
- No `PEXPIREAT`, so AOF-replayed TTLs are relative-to-replay-time, not exact (see
  above).
- No character-class (`[abc]`) support in `KEYS` glob matching — `*`/`?` only.
- No set/sorted-set data types — string, list, and (as of this session) hash only.
- No replication, clustering, pub/sub, or scripting.
- No epoll/kqueue event-loop option — thread-per-connection only.
- No auth/ACLs.

## My understanding of the project

Yes — I've now read through every source file in `src/` (store, hash table, value,
persistence, RESP protocol, dispatcher, TCP server, AOF, main) plus representative
test files and the full README, and made three substantial additions myself this
session (Hash type, AOF persistence, KEYS glob matching), each with new test
coverage, verified under a plain build, ASan/UBSan, and TSan, plus a manual
socket-level smoke test of the running server (hash commands, glob `KEYS`, and a
simulated restart-with-AOF-replay). I understand the concurrency model end to end
well enough to have caught and fixed a real race condition in code I wrote this
session, not just inherited ones. Happy to go deeper on any specific area (e.g. the
RESP wire format in `resp_value.cpp`, or walking through `dispatcher.cpp` command by
command) if useful.

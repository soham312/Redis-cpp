// Store is the concurrency-safe, typed API on top of the raw HashTable —
// the layer commands (and, in a later stage, the TCP server) talk to.
// This mirrors the original Go implementation's Store closely; see
// store.cpp for the per-method rationale where it's specific to a given
// operation.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "hash_table.h"
#include "value.h"

namespace goredis {

// StoreError is returned alongside a Result<T> when an operation is
// well-formed but semantically invalid — currently just the WRONGTYPE
// case (e.g. GET on a key holding a list). This is deliberately not a C++
// exception: exceptions are for exceptional, rarely-hit conditions, and a
// client sending GET against the wrong key type is an entirely ordinary,
// expected outcome a caller needs to branch on — modeling it as a return
// value keeps that branch visible at the call site instead of hidden in a
// try/catch, and avoids exception-handling overhead on what can be a hot
// path.
enum class StoreError {
  kNone,
  kWrongType,
};

// Result<T> is the C++ analog of the original Go implementation's
// (value, found, error) multi-return: value is present only when the key
// existed and no error occurred, and Ok() collapses the common "did this
// succeed" check into one call.
template <typename T>
struct Result {
  std::optional<T> value;
  StoreError error = StoreError::kNone;

  bool Ok() const { return error == StoreError::kNone; }
};

// SnapshotEntry is the atomic unit Store::Snapshot()/LoadSnapshot()
// exchange with the persistence layer (see persistence/rdb.h) — an
// in-memory, self-contained copy of one key, deliberately shaped so the
// persistence layer never needs to touch Store's internals (HashTable,
// mu_, Value) directly, only this public exchange format.
//
// expires_at_wall is a *wall-clock* (system_clock) timestamp, not the
// steady_clock timestamp Value::expires_at actually uses internally.
// That conversion is the whole point of this struct existing: a
// steady_clock time_point has no meaning across a process restart (its
// epoch is unspecified — see the note on Value::expires_at), so anything
// meant to survive being written to disk and read back later has to be
// expressed in wall-clock terms instead. nullopt means no TTL.
struct SnapshotEntry {
  std::string key;
  ValueType type = ValueType::kString;
  std::string str_value;                  // meaningful iff type == kString.
  std::vector<std::string> list_value;    // meaningful iff type == kList.
  std::vector<std::pair<std::string, std::string>> hash_value;  // meaningful iff type == kHash.
  std::optional<std::chrono::system_clock::time_point> expires_at_wall;
};

class Store {
 public:
  // sweep_interval controls how often the background thread actively
  // scans for and removes expired keys (see SweepExpired). Defaulted to
  // Redis's own active-expire-cycle cadence (10x/sec); exposed as a
  // constructor parameter mainly so tests can use a much shorter interval
  // instead of sleeping for hundreds of milliseconds to observe a sweep.
  explicit Store(std::chrono::milliseconds sweep_interval = std::chrono::milliseconds(100));
  ~Store();

  // Store now owns a live background thread that captures `this` in its
  // loop, plus a condition_variable/mutex pair used to signal it to stop.
  // None of that can be safely copied or moved (a moved-from Store would
  // leave the thread pointing at a dead object), and nothing in this
  // project needs to copy or move a Store, so both are simply deleted
  // rather than implemented for a case that never arises.
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  // Set stores key as a string value, overwriting whatever was there
  // before (including a different type — like Redis, SET always succeeds
  // and replaces the key wholesale).
  void Set(const std::string& key, const std::string& value);

  // Get returns the string value for key. An empty (nullopt) Result value
  // with StoreError::kNone means the key simply wasn't found;
  // StoreError::kWrongType means it held a non-string value.
  Result<std::string> Get(const std::string& key) const;

  // Del removes each of keys and returns how many were actually present.
  // (The C++ analog of Go's variadic `Del(keys ...string) int` is a
  // vector parameter — C++ variadic templates would work too, but add
  // complexity a plain vector doesn't need here.)
  int Del(const std::vector<std::string>& keys);

  // Exists returns how many of keys are present (duplicates counted, same
  // as Redis's own EXISTS).
  int Exists(const std::vector<std::string>& keys) const;

  // Keys returns a snapshot of every key currently in the store.
  std::vector<std::string> Keys() const;

  // FlushAll removes every key, resetting the store to empty.
  void FlushAll();

  // Expire sets key to expire `seconds` from now, and returns true if key
  // exists (false if it doesn't — including a key that has already
  // expired but not yet been swept). Matching Redis's own EXPIRE, a
  // non-positive `seconds` deletes the key immediately rather than
  // setting a time in the past.
  bool Expire(const std::string& key, long long seconds);

  // TTL returns the remaining lifetime of key, in whole seconds rounded
  // to the nearest second: -2 if key doesn't exist (or has expired),
  // -1 if key exists but has no TTL set, otherwise the seconds remaining.
  // Matches the return-value convention of Redis's own TTL command.
  long long TTL(const std::string& key) const;

  // SetMaxMemory sets an approximate memory budget in bytes for the
  // store's contents (see EstimateEntryBytes for what's counted). 0 (the
  // default) means unlimited, matching Redis's own `maxmemory 0`
  // convention. Lowering the budget below what's currently used triggers
  // an immediate eviction pass rather than waiting for the next write.
  void SetMaxMemory(std::size_t max_memory_bytes);

  // UsedMemory returns the store's current approximate memory usage in
  // bytes.
  std::size_t UsedMemory() const;

  // LPush prepends values to the list at key (creating it if absent) and
  // returns the resulting length. Values are pushed one at a time in the
  // order given, so the *last* argument ends up at the head — matching
  // Redis's own LPUSH.
  Result<int> LPush(const std::string& key, const std::vector<std::string>& values);

  // RPush appends values to the list at key (creating it if absent) and
  // returns the resulting length.
  Result<int> RPush(const std::string& key, const std::vector<std::string>& values);

  // LLen returns the length of the list at key (0 if the key doesn't
  // exist).
  Result<int> LLen(const std::string& key) const;

  // LRange returns the elements of the list at key between start and stop
  // inclusive, supporting Redis-style negative indices meaning "from the
  // end" (-1 is the last element). A missing key yields an empty vector,
  // not an error — matching Redis's own LRANGE.
  Result<std::vector<std::string>> LRange(const std::string& key, int start, int stop) const;

  // HSet sets each (field, value) pair on the hash at key (creating it if
  // absent) and returns how many of them were *new* fields — matching
  // Redis's own HSET, which counts additions, not updates, in its
  // integer reply.
  Result<int> HSet(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fields);

  // HGet returns the value of one field on the hash at key. A missing key
  // or a missing field are indistinguishable to the caller (both yield an
  // empty Result), same as Redis's own HGET.
  Result<std::string> HGet(const std::string& key, const std::string& field) const;

  // HDel removes each of fields from the hash at key and returns how many
  // were actually present. If this empties the hash entirely, the key
  // itself is deleted — matching Redis's own behavior of never leaving a
  // zero-field hash sitting in the keyspace.
  Result<int> HDel(const std::string& key, const std::vector<std::string>& fields);

  // HGetAll returns every (field, value) pair in the hash at key. A
  // missing key yields an empty vector, not an error.
  Result<std::vector<std::pair<std::string, std::string>>> HGetAll(const std::string& key) const;

  // HLen returns the number of fields in the hash at key (0 if the key
  // doesn't exist).
  Result<int> HLen(const std::string& key) const;

  // HExists reports whether field exists in the hash at key, as 0/1 —
  // matching Redis's own HEXISTS integer reply convention.
  Result<int> HExists(const std::string& key, const std::string& field) const;

  // HKeys/HVals return just the field names, or just the values, of the
  // hash at key.
  Result<std::vector<std::string>> HKeys(const std::string& key) const;
  Result<std::vector<std::string>> HVals(const std::string& key) const;

  // Snapshot captures every live (non-expired) key in one atomic pass —
  // a self-contained, in-memory point-in-time copy of the whole dataset,
  // meant to be handed to the persistence layer (see persistence/rdb.h)
  // to write out to disk. Store has no file I/O of its own; this is the
  // full extent of what it knows about persistence.
  std::vector<SnapshotEntry> Snapshot() const;

  // LoadSnapshot replaces the store's entire contents with entries in one
  // atomic pass — the inverse of Snapshot(), used to restore what a prior
  // Snapshot() (via a loaded RDB file) captured. An entry whose
  // expires_at_wall is already in the past (i.e. it should have expired
  // during whatever time elapsed since it was captured — most commonly,
  // however long the process was down) is silently dropped rather than
  // loaded with a fresh countdown, matching Redis's own RDB-load
  // behavior.
  void LoadSnapshot(std::vector<SnapshotEntry> entries);

 private:
  using Clock = std::chrono::steady_clock;

  // GetOrCreateList fetches key's list Value, creating a new empty one if
  // absent. An existing-but-expired entry is treated the same as absent
  // (deleted and replaced with a fresh list) rather than resurrected.
  // Returns nullptr with *error set to kWrongType if key holds a string.
  // Caller must hold mu_ for writing.
  Value* GetOrCreateList(const std::string& key, StoreError* error);

  // GetOrCreateHash is GetOrCreateList's mirror for the kHash type — see
  // its comment for the shared reasoning (expired-entry handling,
  // WRONGTYPE detection, caller locking requirement).
  Value* GetOrCreateHash(const std::string& key, StoreError* error);

  // IsExpired reports whether v's TTL (if any) has passed. Doesn't itself
  // touch the table — see DeleteIfExpired for the half that does.
  static bool IsExpired(const Value& v);

  // DeleteIfExpired re-checks key under its own write lock and deletes it
  // if still expired, returning whether it did. This is the "escalate"
  // half of the read-path pattern used throughout this class: a read
  // first checks expiry under a cheap shared lock (the common case: not
  // expired, no escalation needed), and only calls this — which takes a
  // full unique lock — on the rarer path where the entry it just saw had
  // already expired. Re-checking under the write lock (rather than
  // deleting unconditionally) matters because another thread could have
  // already deleted, overwritten, or refreshed the key in between the two
  // lock acquisitions. const because "physically remove a tombstone
  // nobody can legitimately observe as live" doesn't change what a caller
  // reading the Store can see — see the note on data_ below.
  bool DeleteIfExpired(const std::string& key) const;

  // SweepExpired is the background thread's per-tick work: scan the
  // whole table for expired keys and remove them. This is what makes
  // expiration *active* rather than purely lazy — a key with a TTL that
  // nothing ever reads again still gets reclaimed instead of sitting in
  // memory forever waiting for a read that never comes.
  void SweepExpired();

  // SweeperRun is the background thread's entry point: call SweepExpired
  // every sweep_interval_ until told to stop.
  void SweeperRun();

  // EstimateEntryBytes approximates how much memory one (key, value)
  // entry occupies: the raw key/payload bytes plus a fixed per-entry
  // overhead standing in for the HashTable Node object itself and
  // typical heap-allocator bookkeeping. This is deliberately an
  // approximation, not exact accounting — an exact count would mean
  // instrumenting every allocation with a custom allocator, which is far
  // more machinery than a portfolio store's eviction policy needs. Real
  // Redis makes the same tradeoff with its own `used_memory` figure.
  static std::size_t EstimateEntryBytes(const std::string& key, const Value& v);

  // Touch records that v was just read or written, for approximate-LRU
  // purposes — stamping it with the next tick of access_clock_ (see
  // below). Safe to call while holding only a shared lock — see the
  // comment on Value::last_accessed_seq for why. Not static (unlike
  // IsExpired/EstimateEntryBytes) because it now needs access_clock_,
  // which belongs to a specific Store instance.
  void Touch(const Value& v) const;

  // EvictIfOverBudget repeatedly evicts the apparent-least-recently-used
  // key from a small random sample (see HashTable::SampleKeys) until
  // used_memory_bytes_ is back at or under max_memory_bytes_, or the
  // table is empty. A no-op when max_memory_bytes_ is 0 (unlimited).
  // Caller must already hold mu_ for writing — this is always invoked as
  // the tail end of a write that could have grown memory usage (Set,
  // LPush, RPush) or of lowering the budget itself (SetMaxMemory).
  void EvictIfOverBudget();

  // mutable so const methods (Get, Exists, Keys, LLen, LRange, TTL) can
  // take a shared (read) lock — locking itself isn't a logical mutation
  // of the Store from the caller's point of view, but
  // std::shared_mutex::lock_shared() is a non-const method, so the mutex
  // member has to be mutable for those methods to remain const.
  //
  // data_ is mutable for the same kind of reason, one level up: those
  // same const read methods need to be able to physically delete an
  // entry they discover has expired (via DeleteIfExpired). That's a
  // "logically const" mutation — from any caller's point of view, a read
  // of an expired key returns exactly the same thing (not found) whether
  // or not the stale entry has been physically swept from the table yet.
  mutable std::shared_mutex mu_;
  mutable HashTable<Value> data_;

  // access_clock_ is Store's logical clock for approximate-LRU: Touch()
  // stamps a Value with the next tick, so ordering ticks is equivalent to
  // ordering accesses exactly, with no dependency on wall-clock
  // resolution (see the comment on Value::last_accessed_seq for why that
  // matters). mutable for the same reason as data_/used_memory_bytes_:
  // const read paths (Get, LLen, LRange) need to advance it too.
  mutable std::atomic<std::uint64_t> access_clock_{0};

  // Background sweeper thread state. sweeper_mu_/sweeper_cv_ are a
  // dedicated pair used only to sleep-with-early-wakeup between sweeps
  // and to signal shutdown — deliberately separate from mu_ (the data
  // lock) so the sweeper thread never has to hold both at once.
  std::chrono::milliseconds sweep_interval_;
  std::thread sweeper_thread_;
  std::mutex sweeper_mu_;
  std::condition_variable sweeper_cv_;
  bool stop_sweeper_ = false;

  // kSampleSize mirrors Redis's own default `maxmemory-samples` — how
  // many random candidates EvictIfOverBudget looks at before evicting the
  // oldest of them. Larger would make eviction closer to exact LRU at the
  // cost of more work per eviction; 5 is the same "good enough"
  // trade-off point Redis itself defaults to.
  static constexpr std::size_t kSampleSize = 5;

  // Approximate per-entry overhead used by EstimateEntryBytes: a rough
  // stand-in for HashTable's Node object and heap-allocator bookkeeping
  // (kPerEntryOverheadBytes), and for one std::string's own object/heap
  // overhead as a list element (kPerListElementOverheadBytes).
  static constexpr std::size_t kPerEntryOverheadBytes = 48;
  static constexpr std::size_t kPerListElementOverheadBytes = 16;
  // Same conservative per-element estimate as kPerListElementOverheadBytes,
  // named separately so a hash field's accounting reads as what it is
  // rather than borrowing a list-specific-sounding constant.
  static constexpr std::size_t kPerHashFieldOverheadBytes = 16;

  // max_memory_bytes_ == 0 means unlimited. Both fields are guarded by
  // mu_ like the rest of Store's mutable state (used_memory_bytes_ isn't
  // atomic on its own — every read or write of it already happens while
  // mu_ is held for one reason or another). used_memory_bytes_ is
  // mutable for the same "logically const" reason as data_: the const
  // read methods' lazy-expiry path (DeleteIfExpired) adjusts it when it
  // physically removes a stale entry.
  std::size_t max_memory_bytes_ = 0;
  mutable std::size_t used_memory_bytes_ = 0;
};

}  // namespace goredis

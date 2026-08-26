// Store is the concurrency-safe, typed API on top of the raw HashTable —
// the layer commands (and, in a later stage, the TCP server) talk to.
// This mirrors the original Go implementation's Store closely; see
// store.cpp for the per-method rationale where it's specific to a given
// operation.
#pragma once

#include <chrono>
#include <condition_variable>
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

 private:
  using Clock = std::chrono::steady_clock;

  // GetOrCreateList fetches key's list Value, creating a new empty one if
  // absent. An existing-but-expired entry is treated the same as absent
  // (deleted and replaced with a fresh list) rather than resurrected.
  // Returns nullptr with *error set to kWrongType if key holds a string.
  // Caller must hold mu_ for writing.
  Value* GetOrCreateList(const std::string& key, StoreError* error);

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

  // Background sweeper thread state. sweeper_mu_/sweeper_cv_ are a
  // dedicated pair used only to sleep-with-early-wakeup between sweeps
  // and to signal shutdown — deliberately separate from mu_ (the data
  // lock) so the sweeper thread never has to hold both at once.
  std::chrono::milliseconds sweep_interval_;
  std::thread sweeper_thread_;
  std::mutex sweeper_mu_;
  std::condition_variable sweeper_cv_;
  bool stop_sweeper_ = false;
};

}  // namespace goredis

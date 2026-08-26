#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace goredis {

// ValueType identifies which member of Value is meaningful. A tagged
// struct (an enum discriminator plus one field per type) is used instead
// of std::variant<std::string, std::vector<std::string>> to keep this
// close to the original Go implementation's Value (a tagged struct, since
// Go has no sum type either) and because command handling wants to switch
// on "what type is this" and return a WRONGTYPE-style error for a
// mismatch — a plain enum switch reads more directly than
// std::visit/std::get<T> for that.
enum class ValueType {
  kString,
  kList,
};

// Value is not independently heap-allocated or wrapped in a smart
// pointer of its own: it lives embedded directly inside a HashTable Node,
// which is what owns the memory (via that Node's std::unique_ptr chain —
// see hash_table.h). Wrapping Value in a second smart pointer here would
// be an extra, pointless allocation for something that already has a
// single, clear owner.
struct Value {
  ValueType type = ValueType::kString;
  std::string str_value;
  std::vector<std::string> list_value;

  // expires_at is nullopt for a key with no TTL. When set, the key is
  // logically gone once steady_clock::now() passes it, even before
  // anything has physically removed the entry from the table (see
  // Store's lazy-expiration and background-sweep logic).
  //
  // steady_clock (not system_clock) deliberately: this is a *duration*
  // from "now" to "expiry", and steady_clock is monotonic — immune to
  // wall-clock adjustments (NTP sync, DST, a user changing the system
  // clock) that would otherwise make a key expire early/late or even
  // appear to un-expire. The tradeoff is that a steady_clock time_point
  // has no meaning across a process restart (its epoch is unspecified),
  // so Stage 4 (persistence) will need to convert to/from an absolute
  // wall-clock timestamp when serializing a key's remaining TTL.
  std::optional<std::chrono::steady_clock::time_point> expires_at;

  // last_accessed_seq backs Store's approximate-LRU eviction: not a
  // timestamp, but a stamp from Store's own logical clock (a counter
  // incremented on every access) recording *when, relative to every other
  // access*, this entry was last read or written. Eviction picks a random
  // sample of keys and evicts whichever has the smallest stamp — the
  // "least recently used" one, in access order rather than wall-clock
  // time.
  //
  // A logical counter instead of a wall-clock timestamp deliberately: an
  // earlier version of this used a millisecond steady_clock reading, and
  // under a burst of same-millisecond operations (entirely realistic
  // under real load — many stores can happen faster than 1ms apart) most
  // entries ended up with identical timestamps, leaving eviction with no
  // real signal to distinguish hot from cold keys. A counter can never
  // tie two *different* accesses — every touch gets a strictly larger
  // value than the one before it, no matter how close together in time
  // they happen — so recency ordering stays exact regardless of clock
  // resolution or throughput. Default-initialized to 0; every code path
  // that creates a new entry is responsible for calling Store::Touch on
  // it immediately (see Store::Set, Store::GetOrCreateList), same as any
  // other access would.
  //
  // std::atomic, not a plain std::uint64_t, specifically so a read
  // (Store::Get and friends, which only hold a *shared* lock so several
  // threads can be in here at once) can still record "this key was just
  // accessed" without a data race — a plain field would need the
  // exclusive lock to update safely, which would serialize every read and
  // defeat the whole point of using a shared_mutex in Store. Concurrent
  // stores to this field from concurrent readers are fine: whichever one
  // lands last wins, and for an *approximate* recency signal that's an
  // acceptable outcome, not a correctness bug.
  //
  // mutable: recording an access is exactly the kind of "logically const"
  // mutation used elsewhere in this project (see Store's data_/mu_) —
  // reading a key doesn't change what a caller observes, but Store's read
  // paths only hold a *shared* lock and hand back `const Value&`/`const
  // Value*`, so touching this field at all requires stripping constness
  // for this one member specifically rather than the whole object.
  mutable std::atomic<std::uint64_t> last_accessed_seq{0};

  Value() = default;

  // std::atomic is neither copyable nor movable by construction (moving
  // one isn't itself atomic), which would otherwise make the compiler
  // implicitly delete Value's move constructor/assignment too — and
  // HashTable relies on Value being movable (Set() moves a new Value in;
  // an update move-assigns over the existing one). So both are written by
  // hand: everything else moves normally, and the atomic's underlying
  // int64 is copied out with a relaxed load. Relaxed is sufficient here
  // because a genuine move implies the source has no other concurrent
  // accessor left (the type's copy is likewise deleted below, so nothing
  // else can be observing `other` mid-move).
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;

  Value(Value&& other) noexcept
      : type(other.type),
        str_value(std::move(other.str_value)),
        list_value(std::move(other.list_value)),
        expires_at(other.expires_at),
        last_accessed_seq(other.last_accessed_seq.load(std::memory_order_relaxed)) {}

  Value& operator=(Value&& other) noexcept {
    type = other.type;
    str_value = std::move(other.str_value);
    list_value = std::move(other.list_value);
    expires_at = other.expires_at;
    last_accessed_seq.store(other.last_accessed_seq.load(std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
  }
};

}  // namespace goredis

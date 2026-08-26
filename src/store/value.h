#pragma once

#include <chrono>
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
};

}  // namespace goredis

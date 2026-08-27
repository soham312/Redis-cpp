#include "server/dispatcher.h"

#include <cctype>
#include <charconv>
#include <limits>
#include <system_error>
#include <utility>

#include "server/aof.h"

namespace goredis {

namespace {

std::string ToUpper(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string ToLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

RespValue WrongArgsError(const std::string& cmd_original) {
  return RespValue::Error("ERR wrong number of arguments for '" + ToLower(cmd_original) + "' command");
}

RespValue WrongTypeError() {
  return RespValue::Error("WRONGTYPE Operation against a key holding the wrong kind of value");
}

RespValue NotIntegerError() { return RespValue::Error("ERR value is not an integer or out of range"); }

bool ParseInt64Arg(const std::string& s, std::int64_t* out) {
  if (s.empty()) {
    return false;
  }
  auto result = std::from_chars(s.data(), s.data() + s.size(), *out);
  return result.ec == std::errc() && result.ptr == s.data() + s.size();
}

// ClampToInt narrows a parsed 64-bit argument (LRANGE's start/stop) down
// to int (what Store::LRange actually takes) by saturating rather than
// truncating — a client passing an index far outside any real list's
// bounds should behave like "far outside," not wrap around via undefined
// or implementation-defined narrowing behavior.
int ClampToInt(std::int64_t v) {
  if (v > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  if (v < std::numeric_limits<int>::min()) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(v);
}

RespValue StringsToBulkArray(const std::vector<std::string>& items) {
  std::vector<RespValue> out;
  out.reserve(items.size());
  for (const auto& item : items) {
    out.push_back(RespValue::BulkString(item));
  }
  return RespValue::Array(std::move(out));
}

// GlobMatch implements a restricted form of Redis's own KEYS pattern
// matching: '*' matches any run of characters (including zero), '?'
// matches exactly one character, and everything else matches itself
// literally. Character classes ('[a-z]') aren't supported — real Redis's
// stringmatchlen does, but nothing in this project's test suite or
// intended usage needs them, and a partial/buggy bracket implementation
// would be worse than an honest, documented subset. The two-pointer
// backtracking algorithm below is the standard technique for wildcard
// matching with a single '*' operator: star_p/star_t remember the most
// recent '*' and how far into text it had matched, so a later mismatch
// can retry that '*' consuming one more character instead of failing
// outright — O(pattern.size() * text.size()) worst case, plenty fast for
// key patterns.
bool GlobMatch(const std::string& pattern, const std::string& text) {
  std::size_t p = 0, t = 0;
  std::size_t star_p = std::string::npos, star_t = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star_p = p;
      star_t = t;
      ++p;
    } else if (star_p != std::string::npos) {
      p = star_p + 1;
      t = ++star_t;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

}  // namespace

RespValue Dispatch(Store& store, const std::vector<std::string>& args, AofWriter* aof) {
  const std::string cmd = ToUpper(args[0]);

  if (cmd == "PING") {
    if (args.size() == 1) {
      return RespValue::SimpleString("PONG");
    }
    if (args.size() == 2) {
      return RespValue::BulkString(args[1]);
    }
    return WrongArgsError(args[0]);
  }

  if (cmd == "ECHO") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    return RespValue::BulkString(args[1]);
  }

  if (cmd == "SET") {
    // Supports the plain form and one option — `SET key value EX
    // seconds` — since Store already exposes Set and Expire as separate
    // primitives and combining them here is what makes a TTL-aware store
    // actually usable from a real client. NX/XX/PX/KEEPTTL and friends
    // are deliberately not implemented — Store has no primitive for
    // "only if absent"/"only if present" semantics, and adding one
    // purely to support an option this project doesn't otherwise need
    // would be scope creep this stage doesn't call for.
    if (args.size() != 3 && args.size() != 5) {
      return WrongArgsError(args[0]);
    }
    if (args.size() == 5) {
      if (ToUpper(args[3]) != "EX") {
        return RespValue::Error("ERR syntax error");
      }
      std::int64_t seconds;
      if (!ParseInt64Arg(args[4], &seconds)) {
        return NotIntegerError();
      }
      store.Set(args[1], args[2]);
      store.Expire(args[1], seconds);
      if (aof != nullptr) {
        aof->Append(args);
      }
      return RespValue::SimpleString("OK");
    }
    store.Set(args[1], args[2]);
    if (aof != nullptr) {
      aof->Append(args);
    }
    return RespValue::SimpleString("OK");
  }

  if (cmd == "GET") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    auto result = store.Get(args[1]);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    if (!result.value.has_value()) {
      return RespValue::NilBulkString();
    }
    return RespValue::BulkString(*result.value);
  }

  if (cmd == "DEL") {
    if (args.size() < 2) {
      return WrongArgsError(args[0]);
    }
    std::vector<std::string> keys(args.begin() + 1, args.end());
    int removed = store.Del(keys);
    if (aof != nullptr) {
      aof->Append(args);
    }
    return RespValue::Integer(removed);
  }

  if (cmd == "EXISTS") {
    if (args.size() < 2) {
      return WrongArgsError(args[0]);
    }
    std::vector<std::string> keys(args.begin() + 1, args.end());
    return RespValue::Integer(store.Exists(keys));
  }

  if (cmd == "KEYS") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    // Store::Keys() has no filtering of its own — everything live is
    // fetched and then matched here against the requested pattern (see
    // GlobMatch above).
    std::vector<std::string> matched;
    for (const auto& key : store.Keys()) {
      if (GlobMatch(args[1], key)) {
        matched.push_back(key);
      }
    }
    return StringsToBulkArray(matched);
  }

  if (cmd == "FLUSHALL") {
    if (args.size() != 1) {
      return WrongArgsError(args[0]);
    }
    store.FlushAll();
    if (aof != nullptr) {
      aof->Append(args);
    }
    return RespValue::SimpleString("OK");
  }

  if (cmd == "EXPIRE") {
    if (args.size() != 3) {
      return WrongArgsError(args[0]);
    }
    std::int64_t seconds;
    if (!ParseInt64Arg(args[2], &seconds)) {
      return NotIntegerError();
    }
    bool existed = store.Expire(args[1], seconds);
    // Logged unconditionally, even when the key didn't exist: replaying
    // an EXPIRE against a since-recreated or still-missing key is a
    // harmless no-op either way, and Redis's own AOF does the same
    // (it doesn't try to skip "commands that didn't do anything").
    //
    // Note this is *not* how real Redis's AOF handles EXPIRE precisely:
    // Redis rewrites a relative EXPIRE into an absolute PEXPIREAT before
    // logging it, so replay reproduces the exact original deadline
    // regardless of when replay happens. This project has no PEXPIREAT,
    // so a replayed EXPIRE re-arms its TTL relative to replay time (i.e.
    // process restart) rather than the original SET/EXPIRE time — an
    // accepted, explicitly-named simplification, unlike RDB persistence
    // (see rdb.cpp), which does anchor TTLs to absolute wall-clock time
    // exactly.
    if (aof != nullptr) {
      aof->Append(args);
    }
    return RespValue::Integer(existed ? 1 : 0);
  }

  if (cmd == "TTL") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    return RespValue::Integer(store.TTL(args[1]));
  }

  if (cmd == "LPUSH" || cmd == "RPUSH") {
    if (args.size() < 3) {
      return WrongArgsError(args[0]);
    }
    std::vector<std::string> values(args.begin() + 2, args.end());
    auto result = (cmd == "LPUSH") ? store.LPush(args[1], values) : store.RPush(args[1], values);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    if (aof != nullptr) {
      aof->Append(args);
    }
    return RespValue::Integer(*result.value);
  }

  if (cmd == "LLEN") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    auto result = store.LLen(args[1]);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    return RespValue::Integer(*result.value);
  }

  if (cmd == "LRANGE") {
    if (args.size() != 4) {
      return WrongArgsError(args[0]);
    }
    std::int64_t start, stop;
    if (!ParseInt64Arg(args[2], &start) || !ParseInt64Arg(args[3], &stop)) {
      return NotIntegerError();
    }
    auto result = store.LRange(args[1], ClampToInt(start), ClampToInt(stop));
    if (!result.Ok()) {
      return WrongTypeError();
    }
    return StringsToBulkArray(*result.value);
  }

  if (cmd == "HSET") {
    // field/value pairs start at args[2]; at least one pair required, and
    // the trailing arguments must come in complete pairs — matching
    // Redis's own HSET arity rules.
    if (args.size() < 4 || (args.size() % 2) != 0) {
      return WrongArgsError(args[0]);
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve((args.size() - 2) / 2);
    for (std::size_t i = 2; i + 1 < args.size(); i += 2) {
      fields.emplace_back(args[i], args[i + 1]);
    }
    auto result = store.HSet(args[1], fields);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    if (aof != nullptr) {
      aof->Append(args);
    }
    return RespValue::Integer(*result.value);
  }

  if (cmd == "HGET") {
    if (args.size() != 3) {
      return WrongArgsError(args[0]);
    }
    auto result = store.HGet(args[1], args[2]);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    if (!result.value.has_value()) {
      return RespValue::NilBulkString();
    }
    return RespValue::BulkString(*result.value);
  }

  if (cmd == "HDEL") {
    if (args.size() < 3) {
      return WrongArgsError(args[0]);
    }
    std::vector<std::string> fields(args.begin() + 2, args.end());
    auto result = store.HDel(args[1], fields);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    if (aof != nullptr) {
      aof->Append(args);
    }
    return RespValue::Integer(*result.value);
  }

  if (cmd == "HGETALL") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    auto result = store.HGetAll(args[1]);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    std::vector<RespValue> out;
    out.reserve(result.value->size() * 2);
    for (const auto& field_value : *result.value) {
      out.push_back(RespValue::BulkString(field_value.first));
      out.push_back(RespValue::BulkString(field_value.second));
    }
    return RespValue::Array(std::move(out));
  }

  if (cmd == "HLEN") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    auto result = store.HLen(args[1]);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    return RespValue::Integer(*result.value);
  }

  if (cmd == "HEXISTS") {
    if (args.size() != 3) {
      return WrongArgsError(args[0]);
    }
    auto result = store.HExists(args[1], args[2]);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    return RespValue::Integer(*result.value);
  }

  if (cmd == "HKEYS" || cmd == "HVALS") {
    if (args.size() != 2) {
      return WrongArgsError(args[0]);
    }
    auto result = (cmd == "HKEYS") ? store.HKeys(args[1]) : store.HVals(args[1]);
    if (!result.Ok()) {
      return WrongTypeError();
    }
    return StringsToBulkArray(*result.value);
  }

  return RespValue::Error("ERR unknown command '" + args[0] + "'");
}

}  // namespace goredis

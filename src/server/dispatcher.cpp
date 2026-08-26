#include "server/dispatcher.h"

#include <cctype>
#include <charconv>
#include <limits>
#include <system_error>

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

}  // namespace

RespValue Dispatch(Store& store, const std::vector<std::string>& args) {
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
      return RespValue::SimpleString("OK");
    }
    store.Set(args[1], args[2]);
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
    return RespValue::Integer(store.Del(keys));
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
    // Store::Keys() has no filtering of its own, and implementing a
    // general glob matcher (the '*'/'?'/literal-char matching real
    // Redis's KEYS supports) is a separate algorithm this networking
    // stage isn't asking for — so only the by-far most common pattern
    // ("give me everything") is supported, and anything else is an
    // honest, explicit error rather than a silently wrong partial
    // implementation.
    if (args[1] != "*") {
      return RespValue::Error("ERR KEYS only supports the '*' (match-all) pattern in this server");
    }
    return StringsToBulkArray(store.Keys());
  }

  if (cmd == "FLUSHALL") {
    if (args.size() != 1) {
      return WrongArgsError(args[0]);
    }
    store.FlushAll();
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
    return RespValue::Integer(store.Expire(args[1], seconds) ? 1 : 0);
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

  return RespValue::Error("ERR unknown command '" + args[0] + "'");
}

}  // namespace goredis

// RespValue models RESP2 (REdis Serialization Protocol v2) — the wire
// format every Redis client understands, and the format this server's
// replies are encoded in. Not RESP3: RESP3 adds richer types (maps,
// sets, doubles, booleans, out-of-band push messages) that nothing in
// this project's command set needs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace goredis {

// A tagged struct (RespType discriminator + one field per payload shape)
// rather than std::variant — same reasoning as Value in store/value.h:
// this project consistently prefers an explicit, switchable tag over
// std::variant's visitor machinery for a type whose whole purpose is to
// be branched on by its kind.
enum class RespType {
  kSimpleString,  // +OK\r\n
  kError,         // -ERR message\r\n
  kInteger,       // :1000\r\n
  kBulkString,    // $6\r\nfoobar\r\n  (or $-1\r\n when is_null)
  kArray,         // *2\r\n...\r\n     (or *-1\r\n when is_null)
};

struct RespValue {
  RespType type = RespType::kSimpleString;
  std::string str;               // kSimpleString / kError / kBulkString payload.
  std::int64_t integer = 0;      // kInteger payload.
  std::vector<RespValue> array;  // kArray payload.
  bool is_null = false;          // true for a null bulk string or null array.

  static RespValue SimpleString(std::string s);
  static RespValue Error(std::string message);
  static RespValue Integer(std::int64_t n);
  static RespValue BulkString(std::string s);
  static RespValue NilBulkString();
  static RespValue Array(std::vector<RespValue> items);
  static RespValue NilArray();

  // Encode serializes this value to its RESP wire representation.
  std::string Encode() const;
};

}  // namespace goredis

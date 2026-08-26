#include "resp/resp_value.h"

namespace goredis {

RespValue RespValue::SimpleString(std::string s) {
  RespValue v;
  v.type = RespType::kSimpleString;
  v.str = std::move(s);
  return v;
}

RespValue RespValue::Error(std::string message) {
  RespValue v;
  v.type = RespType::kError;
  v.str = std::move(message);
  return v;
}

RespValue RespValue::Integer(std::int64_t n) {
  RespValue v;
  v.type = RespType::kInteger;
  v.integer = n;
  return v;
}

RespValue RespValue::BulkString(std::string s) {
  RespValue v;
  v.type = RespType::kBulkString;
  v.str = std::move(s);
  return v;
}

RespValue RespValue::NilBulkString() {
  RespValue v;
  v.type = RespType::kBulkString;
  v.is_null = true;
  return v;
}

RespValue RespValue::Array(std::vector<RespValue> items) {
  RespValue v;
  v.type = RespType::kArray;
  v.array = std::move(items);
  return v;
}

RespValue RespValue::NilArray() {
  RespValue v;
  v.type = RespType::kArray;
  v.is_null = true;
  return v;
}

std::string RespValue::Encode() const {
  switch (type) {
    case RespType::kSimpleString:
      return "+" + str + "\r\n";
    case RespType::kError:
      return "-" + str + "\r\n";
    case RespType::kInteger:
      return ":" + std::to_string(integer) + "\r\n";
    case RespType::kBulkString:
      if (is_null) {
        return "$-1\r\n";
      }
      return "$" + std::to_string(str.size()) + "\r\n" + str + "\r\n";
    case RespType::kArray: {
      if (is_null) {
        return "*-1\r\n";
      }
      std::string out = "*" + std::to_string(array.size()) + "\r\n";
      for (const auto& item : array) {
        out += item.Encode();
      }
      return out;
    }
  }
  // Unreachable: every RespType is handled above. No `default:` case,
  // deliberately — that would defeat -Wswitch's exhaustiveness check,
  // which is exactly what should catch a missed case if RespType ever
  // grows a new member (e.g. a future RESP3 type).
  return "";
}

}  // namespace goredis

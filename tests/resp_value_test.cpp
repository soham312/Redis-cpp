#include "resp/resp_value.h"

#include <gtest/gtest.h>

#include <vector>

using goredis::RespValue;

TEST(RespValueTest, SimpleString) { EXPECT_EQ(RespValue::SimpleString("OK").Encode(), "+OK\r\n"); }

TEST(RespValueTest, Error) { EXPECT_EQ(RespValue::Error("ERR bad").Encode(), "-ERR bad\r\n"); }

TEST(RespValueTest, Integer) {
  EXPECT_EQ(RespValue::Integer(42).Encode(), ":42\r\n");
  EXPECT_EQ(RespValue::Integer(-7).Encode(), ":-7\r\n");
  EXPECT_EQ(RespValue::Integer(0).Encode(), ":0\r\n");
}

TEST(RespValueTest, BulkString) {
  EXPECT_EQ(RespValue::BulkString("hello").Encode(), "$5\r\nhello\r\n");
  EXPECT_EQ(RespValue::BulkString("").Encode(), "$0\r\n\r\n");
}

TEST(RespValueTest, NilBulkString) { EXPECT_EQ(RespValue::NilBulkString().Encode(), "$-1\r\n"); }

TEST(RespValueTest, NilArray) { EXPECT_EQ(RespValue::NilArray().Encode(), "*-1\r\n"); }

TEST(RespValueTest, EmptyArray) { EXPECT_EQ(RespValue::Array({}).Encode(), "*0\r\n"); }

TEST(RespValueTest, ArrayOfMixedTypes) {
  std::vector<RespValue> items = {RespValue::BulkString("a"), RespValue::Integer(1), RespValue::NilBulkString()};
  EXPECT_EQ(RespValue::Array(items).Encode(), "*3\r\n$1\r\na\r\n:1\r\n$-1\r\n");
}

TEST(RespValueTest, NestedArray) {
  std::vector<RespValue> inner = {RespValue::Integer(1), RespValue::Integer(2)};
  std::vector<RespValue> outer = {RespValue::Array(inner), RespValue::SimpleString("x")};
  EXPECT_EQ(RespValue::Array(outer).Encode(), "*2\r\n*2\r\n:1\r\n:2\r\n+x\r\n");
}

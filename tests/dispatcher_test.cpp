#include "server/dispatcher.h"

#include <gtest/gtest.h>

#include <string>

#include "store/store.h"

using goredis::Dispatch;
using goredis::RespType;
using goredis::Store;

TEST(DispatcherTest, PingWithNoArgsRepliesPong) {
  Store s;
  auto r = Dispatch(s, {"PING"});
  EXPECT_EQ(r.type, RespType::kSimpleString);
  EXPECT_EQ(r.str, "PONG");
}

TEST(DispatcherTest, PingWithArgEchoesIt) {
  Store s;
  auto r = Dispatch(s, {"PING", "hello"});
  EXPECT_EQ(r.type, RespType::kBulkString);
  EXPECT_EQ(r.str, "hello");
}

TEST(DispatcherTest, EchoReturnsArgAsBulkString) {
  Store s;
  auto r = Dispatch(s, {"ECHO", "hi"});
  EXPECT_EQ(r.type, RespType::kBulkString);
  EXPECT_EQ(r.str, "hi");
}

TEST(DispatcherTest, EchoWrongArityIsError) {
  Store s;
  auto r = Dispatch(s, {"ECHO"});
  EXPECT_EQ(r.type, RespType::kError);
}

TEST(DispatcherTest, SetThenGetRoundTrips) {
  Store s;
  auto set_r = Dispatch(s, {"SET", "k", "v"});
  EXPECT_EQ(set_r.type, RespType::kSimpleString);
  EXPECT_EQ(set_r.str, "OK");

  auto get_r = Dispatch(s, {"GET", "k"});
  EXPECT_EQ(get_r.type, RespType::kBulkString);
  EXPECT_FALSE(get_r.is_null);
  EXPECT_EQ(get_r.str, "v");
}

TEST(DispatcherTest, GetOnMissingKeyIsNilBulkString) {
  Store s;
  auto r = Dispatch(s, {"GET", "nope"});
  EXPECT_EQ(r.type, RespType::kBulkString);
  EXPECT_TRUE(r.is_null);
}

TEST(DispatcherTest, SetWithExOptionAppliesTtl) {
  Store s;
  auto r = Dispatch(s, {"SET", "k", "v", "EX", "100"});
  EXPECT_EQ(r.str, "OK");
  auto ttl = Dispatch(s, {"TTL", "k"});
  EXPECT_EQ(ttl.type, RespType::kInteger);
  EXPECT_GT(ttl.integer, 0);
  EXPECT_LE(ttl.integer, 100);
}

TEST(DispatcherTest, SetWithUnsupportedOptionIsSyntaxError) {
  Store s;
  auto r = Dispatch(s, {"SET", "k", "v", "PX", "100"});
  EXPECT_EQ(r.type, RespType::kError);
}

TEST(DispatcherTest, DelReturnsCountActuallyRemoved) {
  Store s;
  Dispatch(s, {"SET", "a", "1"});
  auto r = Dispatch(s, {"DEL", "a", "nope"});
  EXPECT_EQ(r.type, RespType::kInteger);
  EXPECT_EQ(r.integer, 1);
}

TEST(DispatcherTest, ExistsCountsDuplicates) {
  Store s;
  Dispatch(s, {"SET", "a", "1"});
  auto r = Dispatch(s, {"EXISTS", "a", "a", "nope"});
  EXPECT_EQ(r.integer, 2);
}

TEST(DispatcherTest, KeysWithStarPatternListsEverything) {
  Store s;
  Dispatch(s, {"SET", "a", "1"});
  Dispatch(s, {"SET", "b", "2"});
  auto r = Dispatch(s, {"KEYS", "*"});
  EXPECT_EQ(r.type, RespType::kArray);
  EXPECT_EQ(r.array.size(), 2u);
}

TEST(DispatcherTest, KeysWithNonStarPatternIsError) {
  Store s;
  auto r = Dispatch(s, {"KEYS", "a*"});
  EXPECT_EQ(r.type, RespType::kError);
}

TEST(DispatcherTest, FlushallEmptiesStore) {
  Store s;
  Dispatch(s, {"SET", "a", "1"});
  auto r = Dispatch(s, {"FLUSHALL"});
  EXPECT_EQ(r.str, "OK");
  EXPECT_TRUE(s.Keys().empty());
}

TEST(DispatcherTest, ExpireOnMissingKeyReturnsZero) {
  Store s;
  auto r = Dispatch(s, {"EXPIRE", "nope", "10"});
  EXPECT_EQ(r.integer, 0);
}

TEST(DispatcherTest, ExpireOnExistingKeyReturnsOne) {
  Store s;
  Dispatch(s, {"SET", "k", "v"});
  auto r = Dispatch(s, {"EXPIRE", "k", "10"});
  EXPECT_EQ(r.integer, 1);
}

TEST(DispatcherTest, ExpireWithNonIntegerArgIsError) {
  Store s;
  Dispatch(s, {"SET", "k", "v"});
  auto r = Dispatch(s, {"EXPIRE", "k", "soon"});
  EXPECT_EQ(r.type, RespType::kError);
  EXPECT_NE(r.str.find("not an integer"), std::string::npos);
}

TEST(DispatcherTest, TtlOnMissingKeyIsMinusTwo) {
  Store s;
  auto r = Dispatch(s, {"TTL", "nope"});
  EXPECT_EQ(r.integer, -2);
}

TEST(DispatcherTest, LPushAndRPushReturnResultingLength) {
  Store s;
  auto r1 = Dispatch(s, {"RPUSH", "list", "a", "b"});
  EXPECT_EQ(r1.integer, 2);
  auto r2 = Dispatch(s, {"LPUSH", "list", "z"});
  EXPECT_EQ(r2.integer, 3);
}

TEST(DispatcherTest, PushOnStringKeyIsWrongType) {
  Store s;
  Dispatch(s, {"SET", "k", "v"});
  auto r = Dispatch(s, {"RPUSH", "k", "a"});
  EXPECT_EQ(r.type, RespType::kError);
  EXPECT_EQ(r.str.rfind("WRONGTYPE", 0), 0u);
}

TEST(DispatcherTest, LlenReflectsListLength) {
  Store s;
  Dispatch(s, {"RPUSH", "list", "a", "b", "c"});
  auto r = Dispatch(s, {"LLEN", "list"});
  EXPECT_EQ(r.integer, 3);
}

TEST(DispatcherTest, LrangeReturnsBulkStringArray) {
  Store s;
  Dispatch(s, {"RPUSH", "list", "a", "b", "c"});
  auto r = Dispatch(s, {"LRANGE", "list", "0", "-1"});
  ASSERT_EQ(r.type, RespType::kArray);
  ASSERT_EQ(r.array.size(), 3u);
  EXPECT_EQ(r.array[0].str, "a");
  EXPECT_EQ(r.array[2].str, "c");
}

TEST(DispatcherTest, LrangeWithNonIntegerArgIsError) {
  Store s;
  Dispatch(s, {"RPUSH", "list", "a"});
  auto r = Dispatch(s, {"LRANGE", "list", "x", "y"});
  EXPECT_EQ(r.type, RespType::kError);
}

TEST(DispatcherTest, UnknownCommandIsError) {
  Store s;
  auto r = Dispatch(s, {"NOTACOMMAND"});
  EXPECT_EQ(r.type, RespType::kError);
  EXPECT_NE(r.str.find("unknown command"), std::string::npos);
}

TEST(DispatcherTest, WrongArityErrorMentionsTheCommandName) {
  Store s;
  auto r = Dispatch(s, {"GET"});
  EXPECT_EQ(r.type, RespType::kError);
  EXPECT_NE(r.str.find("wrong number of arguments"), std::string::npos);
  EXPECT_NE(r.str.find("'get'"), std::string::npos);
}

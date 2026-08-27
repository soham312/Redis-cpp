#include "server/dispatcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

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

TEST(DispatcherTest, KeysWithGlobPatternMatchesOnlyMatchingKeys) {
  Store s;
  Dispatch(s, {"SET", "apple", "1"});
  Dispatch(s, {"SET", "avocado", "2"});
  Dispatch(s, {"SET", "banana", "3"});
  auto r = Dispatch(s, {"KEYS", "a*"});
  ASSERT_EQ(r.type, RespType::kArray);
  std::vector<std::string> got;
  for (const auto& item : r.array) {
    got.push_back(item.str);
  }
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<std::string>{"apple", "avocado"}));
}

TEST(DispatcherTest, KeysWithQuestionMarkMatchesExactlyOneCharacter) {
  Store s;
  Dispatch(s, {"SET", "cat", "1"});
  Dispatch(s, {"SET", "car", "2"});
  Dispatch(s, {"SET", "cart", "3"});
  auto r = Dispatch(s, {"KEYS", "ca?"});
  ASSERT_EQ(r.type, RespType::kArray);
  std::vector<std::string> got;
  for (const auto& item : r.array) {
    got.push_back(item.str);
  }
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<std::string>{"car", "cat"}));
}

TEST(DispatcherTest, KeysWithNoMatchReturnsEmptyArray) {
  Store s;
  Dispatch(s, {"SET", "a", "1"});
  auto r = Dispatch(s, {"KEYS", "z*"});
  ASSERT_EQ(r.type, RespType::kArray);
  EXPECT_TRUE(r.array.empty());
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

// ---------------------------------------------------------------------
// Hash commands
// ---------------------------------------------------------------------

TEST(DispatcherTest, HsetReturnsCountOfNewFieldsOnly) {
  Store s;
  auto r1 = Dispatch(s, {"HSET", "h", "f1", "v1", "f2", "v2"});
  EXPECT_EQ(r1.integer, 2);
  // f1 already exists (updated, not added); f3 is new.
  auto r2 = Dispatch(s, {"HSET", "h", "f1", "v1-updated", "f3", "v3"});
  EXPECT_EQ(r2.integer, 1);
}

TEST(DispatcherTest, HsetWithOddFieldValueCountIsWrongArity) {
  Store s;
  auto r = Dispatch(s, {"HSET", "h", "f1", "v1", "f2"});
  EXPECT_EQ(r.type, RespType::kError);
}

TEST(DispatcherTest, HgetRoundTripsAndMissingFieldIsNil) {
  Store s;
  Dispatch(s, {"HSET", "h", "f", "v"});
  auto r1 = Dispatch(s, {"HGET", "h", "f"});
  EXPECT_EQ(r1.type, RespType::kBulkString);
  EXPECT_EQ(r1.str, "v");

  auto r2 = Dispatch(s, {"HGET", "h", "nope"});
  EXPECT_EQ(r2.type, RespType::kBulkString);
  EXPECT_TRUE(r2.is_null);
}

TEST(DispatcherTest, HdelRemovesFieldsAndReportsCountActuallyRemoved) {
  Store s;
  Dispatch(s, {"HSET", "h", "f1", "v1", "f2", "v2"});
  auto r = Dispatch(s, {"HDEL", "h", "f1", "nope"});
  EXPECT_EQ(r.integer, 1);
  EXPECT_EQ(Dispatch(s, {"HEXISTS", "h", "f1"}).integer, 0);
}

TEST(DispatcherTest, HdelEmptyingAHashDeletesTheKeyEntirely) {
  Store s;
  Dispatch(s, {"HSET", "h", "f1", "v1"});
  Dispatch(s, {"HDEL", "h", "f1"});
  EXPECT_EQ(Dispatch(s, {"EXISTS", "h"}).integer, 0);
}

TEST(DispatcherTest, HgetallReturnsFieldValuePairsInterleaved) {
  Store s;
  Dispatch(s, {"HSET", "h", "f1", "v1", "f2", "v2"});
  auto r = Dispatch(s, {"HGETALL", "h"});
  ASSERT_EQ(r.type, RespType::kArray);
  ASSERT_EQ(r.array.size(), 4u);
  std::vector<std::string> flat;
  for (const auto& item : r.array) {
    flat.push_back(item.str);
  }
  EXPECT_NE(std::find(flat.begin(), flat.end(), "f1"), flat.end());
  EXPECT_NE(std::find(flat.begin(), flat.end(), "v1"), flat.end());
}

TEST(DispatcherTest, HlenReflectsFieldCount) {
  Store s;
  Dispatch(s, {"HSET", "h", "f1", "v1", "f2", "v2"});
  EXPECT_EQ(Dispatch(s, {"HLEN", "h"}).integer, 2);
  EXPECT_EQ(Dispatch(s, {"HLEN", "nope"}).integer, 0);
}

TEST(DispatcherTest, HexistsReportsFieldPresence) {
  Store s;
  Dispatch(s, {"HSET", "h", "f", "v"});
  EXPECT_EQ(Dispatch(s, {"HEXISTS", "h", "f"}).integer, 1);
  EXPECT_EQ(Dispatch(s, {"HEXISTS", "h", "nope"}).integer, 0);
}

TEST(DispatcherTest, HkeysAndHvalsReturnRespectiveSides) {
  Store s;
  Dispatch(s, {"HSET", "h", "f1", "v1", "f2", "v2"});

  auto keys = Dispatch(s, {"HKEYS", "h"});
  ASSERT_EQ(keys.type, RespType::kArray);
  std::vector<std::string> got_keys;
  for (const auto& item : keys.array) got_keys.push_back(item.str);
  std::sort(got_keys.begin(), got_keys.end());
  EXPECT_EQ(got_keys, (std::vector<std::string>{"f1", "f2"}));

  auto vals = Dispatch(s, {"HVALS", "h"});
  ASSERT_EQ(vals.type, RespType::kArray);
  std::vector<std::string> got_vals;
  for (const auto& item : vals.array) got_vals.push_back(item.str);
  std::sort(got_vals.begin(), got_vals.end());
  EXPECT_EQ(got_vals, (std::vector<std::string>{"v1", "v2"}));
}

TEST(DispatcherTest, HashCommandsOnStringKeyAreWrongType) {
  Store s;
  Dispatch(s, {"SET", "k", "v"});
  auto r = Dispatch(s, {"HSET", "k", "f", "v"});
  EXPECT_EQ(r.type, RespType::kError);
  EXPECT_EQ(r.str.rfind("WRONGTYPE", 0), 0u);
}

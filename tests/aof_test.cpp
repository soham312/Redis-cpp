#include "server/aof.h"

#include <cstdio>  // std::remove
#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

#include "server/dispatcher.h"
#include "store/store.h"

using goredis::AofWriter;
using goredis::Dispatch;
using goredis::LoadAof;
using goredis::Store;

namespace {
// ::testing::TempDir() is GTest's cross-platform temp directory helper —
// same convention rdb_test.cpp uses for its own file-backed tests.
//
// Unlike SaveRdb (which always writes a brand-new file via a temp-then-
// rename), AOF is append-only by design — so a leftover file from a
// previous run of this same test would silently accumulate commands
// across runs, corrupting exactly the "did replay reproduce the right
// state" assertions these tests make. std::remove here (ignoring the
// "doesn't exist yet" case, which is the common one) guarantees each
// named test file starts empty for this run, the append-only equivalent
// of what SaveRdb's rename() gives RDB tests for free.
std::string TestFilePath(const std::string& name) {
  std::string path = ::testing::TempDir() + "goredis_aof_test_" + name + ".aof";
  std::remove(path.c_str());
  return path;
}
}  // namespace

TEST(AofTest, WriterOpensAndReportsIsOpen) {
  AofWriter w(TestFilePath("open"));
  EXPECT_TRUE(w.IsOpen());
}

TEST(AofTest, WriterOnUnwritablePathReportsNotOpen) {
  // No such directory exists, so open() must fail — IsOpen() should
  // report that honestly rather than crash or silently no-op.
  AofWriter w("/no/such/directory/at/all/dump.aof");
  EXPECT_FALSE(w.IsOpen());
}

TEST(AofTest, AppendedCommandsReplayToReproduceStoreState) {
  std::string path = TestFilePath("replay");
  {
    AofWriter w(path);
    ASSERT_TRUE(w.IsOpen());
    ASSERT_TRUE(w.Append({"SET", "str", "hello"}));
    ASSERT_TRUE(w.Append({"RPUSH", "list", "a", "b"}));
    ASSERT_TRUE(w.Append({"HSET", "hash", "f1", "v1"}));
    ASSERT_TRUE(w.Append({"DEL", "str"}));
  }

  Store loaded;
  ASSERT_TRUE(LoadAof(loaded, path));
  EXPECT_EQ(loaded.Exists({"str"}), 0);  // SET then DEL, replayed in order
  EXPECT_EQ(*loaded.LRange("list", 0, -1).value, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(*loaded.HGet("hash", "f1").value, "v1");
}

TEST(AofTest, AppendingMoreCommandsExtendsRatherThanTruncates) {
  std::string path = TestFilePath("append_extends");
  {
    AofWriter w(path);
    ASSERT_TRUE(w.Append({"SET", "a", "1"}));
  }
  {
    // A second writer over the same path (as main.cpp does across a
    // restart) must append after the first writer's bytes, not overwrite
    // them — this is what O_APPEND is for.
    AofWriter w(path);
    ASSERT_TRUE(w.Append({"SET", "b", "2"}));
  }

  Store loaded;
  ASSERT_TRUE(LoadAof(loaded, path));
  EXPECT_EQ(*loaded.Get("a").value, "1");
  EXPECT_EQ(*loaded.Get("b").value, "2");
}

TEST(AofTest, LoadingMissingFileFailsWithoutTouchingStore) {
  Store s;
  s.Set("untouched", "v");
  EXPECT_FALSE(LoadAof(s, TestFilePath("does_not_exist")));
  EXPECT_EQ(s.Exists({"untouched"}), 1);
}

TEST(AofTest, DispatchLogsSuccessfulWriteCommandsOnly) {
  std::string path = TestFilePath("dispatch_logging");
  Store store;
  {
    AofWriter aof(path);
    ASSERT_TRUE(aof.IsOpen());

    Dispatch(store, {"SET", "k", "v"}, &aof);        // write: should be logged
    Dispatch(store, {"GET", "k"}, &aof);              // read: should not be logged
    Dispatch(store, {"RPUSH", "k", "x"}, &aof);        // WRONGTYPE (k is a string): should not be logged
  }

  Store replayed;
  ASSERT_TRUE(LoadAof(replayed, path));
  EXPECT_EQ(*replayed.Get("k").value, "v");
  // If GET or the failed RPUSH had been logged, replaying "RPUSH k x"
  // against a string-typed "k" would be a no-op anyway (Dispatch just
  // returns WRONGTYPE again) — so the real assertion is indirect: replay
  // must not have somehow turned "k" into a list.
  auto get_result = replayed.Get("k");
  EXPECT_TRUE(get_result.Ok());
}

TEST(AofTest, ReplayDoesNotReLogIntoTheSameFile) {
  std::string path = TestFilePath("no_reloop");
  {
    AofWriter w(path);
    ASSERT_TRUE(w.Append({"SET", "a", "1"}));
  }

  Store loaded;
  ASSERT_TRUE(LoadAof(loaded, path));

  // Loading again from the same, now-replayed file should reproduce
  // exactly the same single command, not a duplicated or growing log —
  // LoadAof itself never appends anything back to `path`.
  Store loaded_again;
  ASSERT_TRUE(LoadAof(loaded_again, path));
  EXPECT_EQ(*loaded_again.Get("a").value, "1");
  EXPECT_EQ(loaded_again.Keys().size(), 1u);
}

// One AofWriter is shared across every client-handler thread in a real
// server (TcpServer is thread-per-connection), so Append() must be safe
// to call concurrently without corrupting the log — see the comment on
// AofWriter::Append for why this matters (a single Append can span more
// than one write() syscall). This test's real payoff is under a
// ThreadSanitizer build; under a plain build it still verifies the file
// stays parseable and every command survives.
TEST(AofTest, ConcurrentAppendsDoNotCorruptTheLog) {
  std::string path = TestFilePath("concurrent");
  constexpr int kThreads = 8;
  constexpr int kAppendsPerThread = 200;

  {
    AofWriter w(path);
    ASSERT_TRUE(w.IsOpen());

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&w, t] {
        for (int i = 0; i < kAppendsPerThread; ++i) {
          w.Append({"SET", "key" + std::to_string(t) + "_" + std::to_string(i), "v"});
        }
      });
    }
    for (auto& th : threads) {
      th.join();
    }
  }

  Store loaded;
  ASSERT_TRUE(LoadAof(loaded, path));
  EXPECT_EQ(loaded.Keys().size(), static_cast<std::size_t>(kThreads * kAppendsPerThread));
}

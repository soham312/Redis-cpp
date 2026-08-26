#include "resp/command_parser.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using goredis::CommandParser;

TEST(CommandParserTest, ParsesWholeMultibulkCommandInOneFeed) {
  CommandParser p;
  std::string cmd = "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n";
  p.Feed(cmd.data(), cmd.size());
  std::vector<std::string> args;
  std::string err;
  ASSERT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kComplete);
  EXPECT_EQ(args, (std::vector<std::string>{"GET", "foo"}));
  EXPECT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kIncomplete);
}

TEST(CommandParserTest, HandlesCommandFragmentedByteAtATime) {
  CommandParser p;
  std::string cmd = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
  std::vector<std::string> args;
  std::string err;
  for (std::size_t i = 0; i < cmd.size(); ++i) {
    p.Feed(&cmd[i], 1);
    auto status = p.TryParseCommand(&args, &err);
    if (i + 1 < cmd.size()) {
      ASSERT_EQ(status, CommandParser::Status::kIncomplete) << "at byte " << i;
    } else {
      ASSERT_EQ(status, CommandParser::Status::kComplete);
    }
  }
  EXPECT_EQ(args, (std::vector<std::string>{"SET", "k", "v"}));
}

TEST(CommandParserTest, DrainsMultiplePipelinedCommandsFromOneFeed) {
  CommandParser p;
  std::string cmd = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
  p.Feed(cmd.data(), cmd.size());
  std::vector<std::string> args;
  std::string err;
  ASSERT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kComplete);
  EXPECT_EQ(args, (std::vector<std::string>{"PING"}));
  ASSERT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kComplete);
  EXPECT_EQ(args, (std::vector<std::string>{"PING"}));
  EXPECT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kIncomplete);
}

TEST(CommandParserTest, IncompleteMultibulkLeavesBufferIntactForMoreData) {
  CommandParser p;
  std::string partial = "*2\r\n$3\r\nGET\r\n";  // second bulk string missing entirely
  p.Feed(partial.data(), partial.size());
  std::vector<std::string> args;
  std::string err;
  EXPECT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kIncomplete);

  std::string rest = "$3\r\nfoo\r\n";
  p.Feed(rest.data(), rest.size());
  ASSERT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kComplete);
  EXPECT_EQ(args, (std::vector<std::string>{"GET", "foo"}));
}

TEST(CommandParserTest, RejectsInvalidBulkLength) {
  CommandParser p;
  std::string bad = "*2\r\n$abc\r\nxx\r\n";
  p.Feed(bad.data(), bad.size());
  std::vector<std::string> args;
  std::string err;
  EXPECT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kProtocolError);
  EXPECT_FALSE(err.empty());
}

TEST(CommandParserTest, RejectsNegativeMultibulkLength) {
  CommandParser p;
  std::string bad = "*-5\r\n";
  p.Feed(bad.data(), bad.size());
  std::vector<std::string> args;
  std::string err;
  EXPECT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kProtocolError);
}

TEST(CommandParserTest, RejectsOversizedMultibulkLength) {
  CommandParser p;
  std::string bad = "*99999999\r\n";
  p.Feed(bad.data(), bad.size());
  std::vector<std::string> args;
  std::string err;
  EXPECT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kProtocolError);
}

TEST(CommandParserTest, RejectsMissingCrlfAfterBulkPayload) {
  CommandParser p;
  std::string bad = "*1\r\n$3\r\nabcXX";  // payload followed by garbage instead of CRLF
  p.Feed(bad.data(), bad.size());
  std::vector<std::string> args;
  std::string err;
  EXPECT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kProtocolError);
}

TEST(CommandParserTest, ParsesSimpleInlineCommand) {
  CommandParser p;
  std::string cmd = "PING\r\n";
  p.Feed(cmd.data(), cmd.size());
  std::vector<std::string> args;
  std::string err;
  ASSERT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kComplete);
  EXPECT_EQ(args, (std::vector<std::string>{"PING"}));
}

TEST(CommandParserTest, InlineCommandToleratesExtraWhitespaceAndBareNewline) {
  CommandParser p;
  std::string cmd = "  SET   foo   bar  \n";
  p.Feed(cmd.data(), cmd.size());
  std::vector<std::string> args;
  std::string err;
  ASSERT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kComplete);
  EXPECT_EQ(args, (std::vector<std::string>{"SET", "foo", "bar"}));
}

TEST(CommandParserTest, InlineParserSkipsBlankLinesRatherThanErroring) {
  CommandParser p;
  std::string cmd = "\r\n\r\nPING\r\n";
  p.Feed(cmd.data(), cmd.size());
  std::vector<std::string> args;
  std::string err;
  ASSERT_EQ(p.TryParseCommand(&args, &err), CommandParser::Status::kComplete);
  EXPECT_EQ(args, (std::vector<std::string>{"PING"}));
}

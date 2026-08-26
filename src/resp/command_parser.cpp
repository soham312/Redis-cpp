#include "resp/command_parser.h"

#include <cctype>
#include <charconv>
#include <system_error>

namespace goredis {

namespace {

// ParseInt64 is a strict integer parser: no leading/trailing whitespace,
// no partial matches ("5x" is rejected, not truncated to 5). std::
// from_chars (rather than std::stoll, which throws, or strtoll, which
// silently stops at the first invalid character and reports failure only
// via errno/endptr in a clunkier way) gives exactly this for free — it's
// the standard C++17 tool for exactly this job.
bool ParseInt64(const std::string& s, std::int64_t* out) {
  if (s.empty()) {
    return false;
  }
  auto result = std::from_chars(s.data(), s.data() + s.size(), *out);
  return result.ec == std::errc() && result.ptr == s.data() + s.size();
}

}  // namespace

void CommandParser::Feed(const char* data, std::size_t len) { buffer_.append(data, len); }

CommandParser::Status CommandParser::TryParseCommand(std::vector<std::string>* out_args, std::string* out_error) {
  if (buffer_.empty()) {
    return Status::kIncomplete;
  }
  if (buffer_[0] == '*') {
    return TryParseMultibulk(out_args, out_error);
  }
  return TryParseInline(out_args, out_error);
}

CommandParser::Status CommandParser::TryParseMultibulk(std::vector<std::string>* out_args, std::string* out_error) {
  std::size_t pos = 0;

  std::size_t line_end = buffer_.find("\r\n", pos);
  if (line_end == std::string::npos) {
    return Status::kIncomplete;
  }
  std::string count_str = buffer_.substr(1, line_end - 1);  // skip the leading '*'
  std::int64_t argc = 0;
  if (!ParseInt64(count_str, &argc) || argc <= 0 || argc > kMaxArgs) {
    *out_error = "invalid multibulk length";
    return Status::kProtocolError;
  }
  pos = line_end + 2;

  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));

  for (std::int64_t i = 0; i < argc; ++i) {
    if (pos >= buffer_.size()) {
      return Status::kIncomplete;
    }
    if (buffer_[pos] != '$') {
      *out_error = "expected '$' to start a bulk string argument";
      return Status::kProtocolError;
    }

    std::size_t bulk_line_end = buffer_.find("\r\n", pos);
    if (bulk_line_end == std::string::npos) {
      return Status::kIncomplete;
    }
    std::string len_str = buffer_.substr(pos + 1, bulk_line_end - (pos + 1));
    std::int64_t bulk_len = 0;
    if (!ParseInt64(len_str, &bulk_len) || bulk_len < 0 || bulk_len > kMaxBulkLen) {
      *out_error = "invalid bulk length";
      return Status::kProtocolError;
    }
    pos = bulk_line_end + 2;

    // Need bulk_len bytes of payload plus its trailing CRLF.
    if (pos + static_cast<std::size_t>(bulk_len) + 2 > buffer_.size()) {
      return Status::kIncomplete;
    }
    args.push_back(buffer_.substr(pos, static_cast<std::size_t>(bulk_len)));
    pos += static_cast<std::size_t>(bulk_len);
    if (buffer_[pos] != '\r' || buffer_[pos + 1] != '\n') {
      *out_error = "expected CRLF after bulk string data";
      return Status::kProtocolError;
    }
    pos += 2;
  }

  buffer_.erase(0, pos);
  *out_args = std::move(args);
  return Status::kComplete;
}

CommandParser::Status CommandParser::TryParseInline(std::vector<std::string>* out_args, std::string* out_error) {
  // Loops rather than handling one line per call: a blank inline line
  // (real Redis silently ignores these too) has to be consumed and
  // skipped, not reported as a command — but it also isn't
  // "kIncomplete" in the sense of "buffer untouched, wait for more data,"
  // since it *was* consumed. Looping here keeps that distinction from
  // leaking into the caller, which only ever needs to know "got a
  // command" / "need more data" / "malformed."
  while (true) {
    std::size_t newline_pos = buffer_.find('\n');
    if (newline_pos == std::string::npos) {
      if (buffer_.size() > static_cast<std::size_t>(kMaxBulkLen)) {
        // A client sending an unterminated line would otherwise grow
        // buffer_ forever — the same DoS concern kMaxBulkLen guards
        // against for multibulk requests.
        *out_error = "inline command too long";
        return Status::kProtocolError;
      }
      return Status::kIncomplete;
    }

    std::string line = buffer_.substr(0, newline_pos);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    buffer_.erase(0, newline_pos + 1);

    std::vector<std::string> args;
    std::size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
      }
      if (i >= line.size()) {
        break;
      }
      std::size_t start = i;
      while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
      }
      args.push_back(line.substr(start, i - start));
    }

    if (args.empty()) {
      continue;  // blank line — skip it and look at whatever comes next.
    }

    *out_args = std::move(args);
    return Status::kComplete;
  }
}

}  // namespace goredis

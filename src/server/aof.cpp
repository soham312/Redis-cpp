#include "server/aof.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <iterator>

#include "resp/command_parser.h"
#include "resp/resp_value.h"
#include "server/dispatcher.h"

namespace goredis {

AofWriter::AofWriter(const std::string& path) {
  // O_APPEND, not "open then seek to end": O_APPEND makes every write()
  // atomically append at the file's current end, even if some other
  // process/fd were also writing to it concurrently — the POSIX-correct
  // way to implement a log file, versus a manual seek-then-write which
  // has a race between the two steps. 0644 matches what a plain `>`
  // redirect or SaveRdb's own ofstream would produce.
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
}

AofWriter::~AofWriter() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool AofWriter::Append(const std::vector<std::string>& args) {
  if (fd_ < 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);

  // Encoded as a RESP multibulk array of bulk strings — precisely the
  // same wire shape a real client's request takes (RESP doesn't have a
  // separate "request" vs. "array reply" encoding, only context
  // distinguishes them). Building it via RespValue::Array/BulkString and
  // reusing Encode() means this file needs no encoder of its own, and
  // LoadAof below can replay it by feeding the exact same bytes back
  // through CommandParser that a live client connection would produce.
  std::vector<RespValue> items;
  items.reserve(args.size());
  for (const auto& arg : args) {
    items.push_back(RespValue::BulkString(arg));
  }
  std::string encoded = RespValue::Array(std::move(items)).Encode();

  const char* data = encoded.data();
  std::size_t remaining = encoded.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd_, data, remaining);
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // interrupted by a signal before writing anything — not a real failure, just retry.
      }
      return false;
    }
    data += n;
    remaining -= static_cast<std::size_t>(n);
  }

  // See the comment on Append's declaration for why every call fsyncs
  // rather than batching.
  return ::fsync(fd_) == 0;
}

bool LoadAof(Store& store, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  CommandParser parser;
  parser.Feed(buf.data(), buf.size());

  std::vector<std::string> args;
  std::string parse_error;
  while (true) {
    CommandParser::Status status = parser.TryParseCommand(&args, &parse_error);
    if (status == CommandParser::Status::kIncomplete) {
      break;  // a fully-written AOF file never ends mid-command; leftover bytes here mean a torn/truncated write.
    }
    if (status == CommandParser::Status::kProtocolError) {
      return false;
    }
    // aof (the 3rd argument) is deliberately omitted here: replaying an
    // already-logged command must not re-log it right back into the same
    // file, which would make LoadAof followed by any future save grow
    // the file without bound. The reply Dispatch returns is discarded —
    // there's no client connection to send it to during a replay, only
    // the side effect on `store` matters.
    Dispatch(store, args);
  }
  return true;
}

}  // namespace goredis

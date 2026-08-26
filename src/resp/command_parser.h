// CommandParser incrementally parses client requests off a TCP byte
// stream into commands: a command name plus its arguments, all as plain
// strings (Store's own API, and RESP's own request format, only ever
// deal in strings — interpreting one as e.g. "a valid integer for
// EXPIRE" happens later, in the dispatcher).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace goredis {

// Fed bytes as they arrive from recv() — which may split a single
// command across multiple calls, or deliver several pipelined commands
// in one call — this supports both request shapes real Redis accepts:
//
//   - RESP multibulk arrays (*N\r\n$len\r\n...\r\n...\r\n): what every
//     real client sends. The primary, fully general path.
//   - Inline commands (a bare line of whitespace-separated tokens, e.g.
//     "PING\r\n" or "SET foo bar\r\n"): a deliberately simplified subset
//     of real Redis's own inline protocol (no quoting/escaping support),
//     included purely so this server can be poked by hand with a plain
//     TCP tool like `nc`/telnet without hand-encoding RESP.
//
// Not a resumable/incremental state machine internally: each call to
// TryParseCommand re-parses from the start of whatever's currently
// buffered, only committing (erasing the consumed prefix) once a full
// command is confirmed present. That costs O(one command's size) of
// extra work per partial read rather than O(1) — an accepted
// simplification, since a single command's size is bounded by
// kMaxBulkLen/kMaxArgs anyway, in exchange for not having to persist
// parser state across Feed() calls the way a true incremental parser
// would.
class CommandParser {
 public:
  enum class Status {
    kIncomplete,     // not enough data buffered yet for a full command — call again after the next Feed().
    kComplete,       // *out_args holds one full command; its bytes were removed from the internal buffer.
    kProtocolError,  // *out_error explains why; the connection should be closed — parser state after a
                     // malformed request isn't something this recovers from (matches real Redis).
  };

  // Feed appends newly-received bytes to the internal buffer.
  void Feed(const char* data, std::size_t len);

  // TryParseCommand attempts to extract one complete command from
  // whatever's currently buffered. Callers should call this in a loop
  // after each Feed() until it returns kIncomplete, to drain every
  // pipelined command already sitting in the buffer before waiting for
  // more data from the socket.
  Status TryParseCommand(std::vector<std::string>* out_args, std::string* out_error);

 private:
  Status TryParseMultibulk(std::vector<std::string>* out_args, std::string* out_error);
  Status TryParseInline(std::vector<std::string>* out_args, std::string* out_error);

  std::string buffer_;

  // kMaxArgs/kMaxBulkLen guard against a malicious or buggy client
  // claiming an absurd array length or bulk-string length and forcing
  // this server to allocate unbounded memory before it's even seen that
  // much data — the same class of concern real Redis's own
  // proto-max-bulk-len guards against. Sized generously for this
  // project's scale (nothing here needs multi-hundred-MB values), not
  // tuned to match Redis's own defaults exactly.
  static constexpr std::int64_t kMaxArgs = 1024;
  static constexpr std::int64_t kMaxBulkLen = 64 * 1024 * 1024;  // 64 MiB
};

}  // namespace goredis

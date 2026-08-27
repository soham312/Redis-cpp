// AOF (Append-Only File) persistence: a log of every write command this
// server has executed, replayed in order to reconstruct the dataset on
// startup — this project's second, complementary persistence mechanism
// alongside RDB (see persistence/rdb.h).
//
// RDB and AOF make opposite tradeoffs, and this server can run with
// either, independently (see main.cpp's --aof flag): RDB is a compact
// point-in-time snapshot, cheap to load, but loses everything written
// since the last save if the process dies uncleanly. AOF logs every
// write as it happens, so at most the last fsync's worth of writes is
// ever at risk — at the cost of a file that grows without bound (no
// AOF-rewrite/compaction is implemented here; a real Redis periodically
// rewrites its AOF down to the equivalent of a fresh RDB-style snapshot,
// which is a natural, separate addition this project doesn't currently
// need) and a slower startup (replaying N commands vs. loading one
// snapshot).
//
// Deliberately placed in server/, not persistence/: unlike RDB (which
// only ever touches Store's own Snapshot()/LoadSnapshot() exchange
// format), AOF replay works in terms of *commands* — it has to run each
// logged command back through Dispatch() to reconstruct state, which
// means depending on the command layer. Putting that dependency in
// persistence/ would mean "persistence" depending on "server", inverting
// this project's own layering (see the architecture section of
// README.md); keeping it in server/ alongside the dispatcher it calls
// keeps the dependency graph pointing the same direction it always has.
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "store/store.h"

namespace goredis {

// AofWriter appends one already-successfully-executed command at a time
// to an AOF file, in the exact RESP multibulk wire format a real client
// would have sent it in — see aof.cpp for why that specific encoding was
// chosen (short version: it lets LoadAof reuse CommandParser instead of
// inventing a second, log-specific format).
class AofWriter {
 public:
  // Opens (or creates) path for appending. Nothing is truncated: an
  // existing file's prior contents are preserved and new commands are
  // appended after them, which is the entire point of a log-structured
  // persistence format — see IsOpen() for how to detect a failed open.
  explicit AofWriter(const std::string& path);

  // Non-copyable/non-movable: owns a raw OS file descriptor with no
  // reference counting, and nothing in this project needs more than one
  // AofWriter live for a given file at a time.
  AofWriter(const AofWriter&) = delete;
  AofWriter& operator=(const AofWriter&) = delete;
  AofWriter(AofWriter&&) = delete;
  AofWriter& operator=(AofWriter&&) = delete;

  ~AofWriter();

  // IsOpen reports whether construction actually succeeded — e.g. false
  // if path's directory doesn't exist or isn't writable. A caller that
  // gets false back should treat AOF logging as unavailable for this run
  // rather than crash the whole server over it, the same "a failed save
  // isn't fatal" philosophy SaveRdb already follows.
  bool IsOpen() const { return fd_ >= 0; }

  // Append serializes args as one RESP command and appends+fsyncs it,
  // returning false if either step failed (a full disk, most commonly).
  // Safe to call concurrently: one AofWriter is shared across every
  // client-handler thread (TcpServer is thread-per-connection — see
  // tcp_server.h), so without its own lock, two threads' encoded commands
  // could interleave mid-write() (a single Append can take more than one
  // write() syscall if the kernel does a partial write) and corrupt the
  // log with an interleaved, unparseable byte stream. mu_ below serializes
  // the whole encode-write-fsync sequence per call, the same kind of
  // correctness issue this project's own TcpServer::Stop() comment
  // describes catching via ThreadSanitizer elsewhere.
  //
  // fsync on every single call is the simplest fully-durable policy —
  // matching Redis's own `appendfsync always` — at the cost of one
  // syscall-and-wait per write command. Real Redis defaults to
  // `everysec` (batch fsync once a second) for far better throughput
  // under heavy write load; that would need a background flush thread
  // and a durability window this project's scale doesn't call for, so
  // the simpler, easier-to-reason-about-and-test policy is used instead,
  // the same kind of deliberate scope call this project makes elsewhere
  // (thread-per-connection over epoll, RDB over AOF originally, etc).
  bool Append(const std::vector<std::string>& args);

 private:
  int fd_ = -1;
  std::mutex mu_;
};

// LoadAof replays every command recorded in the AOF file at path against
// store, in the order they were originally logged. Returns false only if
// the file couldn't be opened or contained a wire-format-level protocol
// error (matching CommandParser's own kProtocolError) — an individual
// replayed command that fails at the Store level (e.g. a WRONGTYPE, which
// shouldn't be possible from a log of commands that all succeeded when
// first executed, but could happen from a hand-edited or corrupted file)
// is simply skipped rather than aborting the whole replay, since Dispatch
// itself has no way to signal that upward other than the RespValue it
// returns, which a replay has no client to send it to anyway.
bool LoadAof(Store& store, const std::string& path);

}  // namespace goredis

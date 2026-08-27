// The server executable: wires together everything the earlier stages
// built (Store, RDB persistence) with this stage's networking layer into
// an actual runnable process — load a dataset, serve clients, and save
// it back on a clean shutdown.
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "persistence/rdb.h"
#include "server/aof.h"
#include "server/tcp_server.h"
#include "store/store.h"

namespace {

// volatile sig_atomic_t is the one C++-standard-guaranteed-safe way to
// communicate from a signal handler to the rest of the program: writing
// to it is the only kind of memory access the standard promises won't
// itself be undefined behavior inside a handler. The handler does
// nothing else — no mutex, no I/O, no calling into Store or TcpServer
// directly — because none of that is guaranteed async-signal-safe.
// Instead, main()'s own loop below polls this flag from ordinary thread
// context and does the actual shutdown work there, where every one of
// those operations is perfectly safe.
volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleShutdownSignal(int /*signum*/) { g_shutdown_requested = 1; }

struct Options {
  int port = 6380;  // not 6379, to avoid colliding with a real redis-server during local testing
  std::string rdb_path = "dump.grdb";
  std::string aof_path;               // empty = AOF disabled (the default); see server/aof.h.
  std::size_t max_memory_bytes = 0;  // 0 = unlimited, matching Store's own default
};

Options ParseArgs(int argc, char** argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next_value = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };

    if (arg == "--port") {
      opts.port = std::atoi(next_value().c_str());
    } else if (arg == "--rdb") {
      opts.rdb_path = next_value();
    } else if (arg == "--aof") {
      opts.aof_path = next_value();
    } else if (arg == "--maxmemory") {
      opts.max_memory_bytes = static_cast<std::size_t>(std::atoll(next_value().c_str()));
    } else {
      std::fprintf(stderr, "Unknown argument: %s (expected --port, --rdb, --aof, or --maxmemory)\n", arg.c_str());
    }
  }
  return opts;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts = ParseArgs(argc, argv);

  std::signal(SIGINT, HandleShutdownSignal);
  std::signal(SIGTERM, HandleShutdownSignal);
  // Without this, writing to a socket a client has already closed
  // delivers SIGPIPE, whose default action is to kill the whole process
  // — one misbehaving client would take the entire server down. Ignoring
  // it means that same write instead just fails send() with EPIPE, which
  // TcpServer::SendAll already treats as an ordinary "stop serving this
  // client" condition.
  std::signal(SIGPIPE, SIG_IGN);

  goredis::Store store;
  if (opts.max_memory_bytes > 0) {
    store.SetMaxMemory(opts.max_memory_bytes);
  }

  // RDB and AOF are mutually exclusive persistence modes here, selected
  // by whether --aof was given: exactly one load path runs, and only
  // that mode's writer stays active for the rest of the process. Redis
  // itself can technically run both directions at once (AOF as primary,
  // RDB for point-in-time backups) but that's real added complexity —
  // deciding which one restores authoritative state after a crash where
  // they've diverged — that a portfolio-scale server doesn't need to take
  // on. See server/aof.h's header comment for the fuller RDB-vs-AOF
  // tradeoff.
  std::unique_ptr<goredis::AofWriter> aof_writer;
  if (!opts.aof_path.empty()) {
    if (goredis::LoadAof(store, opts.aof_path)) {
      std::printf("Replayed AOF log from %s\n", opts.aof_path.c_str());
    } else {
      std::printf("No existing AOF log at %s (or it couldn't be read) — starting empty.\n", opts.aof_path.c_str());
    }
    aof_writer = std::make_unique<goredis::AofWriter>(opts.aof_path);
    if (!aof_writer->IsOpen()) {
      std::fprintf(stderr, "Warning: couldn't open %s for AOF logging — writes won't be persisted this run.\n",
                   opts.aof_path.c_str());
      aof_writer.reset();
    }
  } else if (goredis::LoadRdb(store, opts.rdb_path)) {
    std::printf("Loaded dataset from %s\n", opts.rdb_path.c_str());
  } else {
    std::printf("No existing dataset at %s (or it couldn't be loaded) — starting empty.\n", opts.rdb_path.c_str());
  }

  goredis::TcpServer server(store, opts.port, aof_writer.get());
  if (!server.Start()) {
    std::fprintf(stderr, "Failed to start listening on port %d\n", opts.port);
    return 1;
  }

  std::thread accept_thread([&server] { server.AcceptLoop(); });

  std::printf("goredis listening on port %d (Ctrl+C to stop)\n", opts.port);

  while (!g_shutdown_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::printf("\nShutting down...\n");
  server.Stop();
  accept_thread.join();

  // In AOF mode, every write was already durably fsynced as it happened
  // (see AofWriter::Append) — there's nothing left to do at shutdown, no
  // final save step the way RDB mode needs one.
  if (aof_writer == nullptr) {
    if (goredis::SaveRdb(store, opts.rdb_path)) {
      std::printf("Saved dataset to %s\n", opts.rdb_path.c_str());
    } else {
      std::fprintf(stderr, "Warning: failed to save dataset to %s\n", opts.rdb_path.c_str());
    }
  }

  return 0;
}

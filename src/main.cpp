// The server executable: wires together everything the earlier stages
// built (Store, RDB persistence) with this stage's networking layer into
// an actual runnable process — load a dataset, serve clients, and save
// it back on a clean shutdown.
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "persistence/rdb.h"
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
    } else if (arg == "--maxmemory") {
      opts.max_memory_bytes = static_cast<std::size_t>(std::atoll(next_value().c_str()));
    } else {
      std::fprintf(stderr, "Unknown argument: %s (expected --port, --rdb, or --maxmemory)\n", arg.c_str());
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

  if (goredis::LoadRdb(store, opts.rdb_path)) {
    std::printf("Loaded dataset from %s\n", opts.rdb_path.c_str());
  } else {
    std::printf("No existing dataset at %s (or it couldn't be loaded) — starting empty.\n", opts.rdb_path.c_str());
  }

  goredis::TcpServer server(store, opts.port);
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

  if (goredis::SaveRdb(store, opts.rdb_path)) {
    std::printf("Saved dataset to %s\n", opts.rdb_path.c_str());
  } else {
    std::fprintf(stderr, "Warning: failed to save dataset to %s\n", opts.rdb_path.c_str());
  }

  return 0;
}

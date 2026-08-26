#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include "store/store.h"

namespace goredis {

// TcpServer is a minimal, thread-per-connection TCP server: one
// dedicated OS thread per connected client. That mirrors the same
// concurrency philosophy Store itself already uses (plain OS-level
// primitives, no more machinery than the task needs) and is what "raw
// POSIX sockets, concurrent client handling" most directly calls for.
//
// An event-loop design (epoll/kqueue, one thread multiplexing many
// connections) would scale to far more concurrent clients with less
// per-connection memory overhead, but it's also platform-specific (epoll
// is Linux-only, kqueue is BSD/macOS-only — supporting both portably
// means two separate implementations behind an abstraction) and
// substantially more complex machinery than this stage is asking to
// demonstrate, or than a portfolio demo's connection counts would ever
// actually need.
class TcpServer {
 public:
  TcpServer(Store& store, int port);

  // Start creates, binds, and listens on the server's socket. Must
  // succeed before AcceptLoop() is called. Returns false (after printing
  // the specific errno-based reason to stderr) if any step failed — most
  // commonly, the port is already in use. Deliberately synchronous and
  // separate from AcceptLoop(): a caller on the main thread can react to
  // a bind failure immediately, rather than having to synchronize with
  // whatever thread ends up running the (blocking) accept loop.
  bool Start();

  // AcceptLoop blocks, accepting connections and spawning one detached
  // thread per client, until Stop() is called from another thread.
  // Requires Start() to have already succeeded.
  void AcceptLoop();

  // Stop signals AcceptLoop() to stop accepting new connections,
  // force-disconnects every currently-connected client, and waits
  // (bounded) for their handler threads to finish before returning.
  // Must be called from ordinary thread context, not a signal handler —
  // it locks a mutex, which isn't async-signal-safe. (See main.cpp for
  // how this project bridges a Ctrl+C/SIGTERM to this call safely: the
  // signal handler only sets a sig_atomic_t flag, and an ordinary thread
  // polls that flag and calls Stop() itself.)
  void Stop();

 private:
  void HandleClient(int client_fd);

  Store& store_;
  int port_;

  // atomic, not a plain int: Start() writes it (main thread), AcceptLoop()
  // reads it every iteration to call accept() (the accept-loop thread),
  // and Stop() writes it again to -1 (whatever thread calls Stop()) — a
  // plain int here would be a real, unsynchronized data race on the
  // variable itself (caught by ThreadSanitizer), separate from and in
  // addition to the well-known POSIX-level question of whether closing
  // an fd another thread is blocked on is itself well-defined. Relaxed
  // ordering is enough: nothing here depends on this fd's value
  // establishing a happens-before relationship with any other memory —
  // it's just read to pass to a syscall.
  std::atomic<int> listen_fd_{-1};

  std::atomic<bool> stop_flag_{false};
  std::atomic<int> active_clients_{0};

  // Tracks every currently-connected client's fd so Stop() can
  // force-disconnect them (via shutdown(), not close() — see
  // tcp_server.cpp for why that distinction matters for a fd another
  // thread may be blocked on).
  std::mutex clients_mu_;
  std::vector<int> client_fds_;
};

}  // namespace goredis

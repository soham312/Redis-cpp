#include "server/tcp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

#include "resp/command_parser.h"
#include "server/dispatcher.h"

namespace goredis {

namespace {

constexpr std::size_t kRecvBufSize = 4096;
constexpr int kListenBacklog = 128;

// SendAll loops until every byte of data has been handed to the kernel,
// or the connection fails. A single send() call is not guaranteed to
// accept the whole buffer — TCP sockets can do partial writes — so
// looping here is required for correctness, not just defensiveness.
bool SendAll(int fd, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (n <= 0) {
      return false;  // connection error or closed — caller stops serving this client.
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

TcpServer::TcpServer(Store& store, int port) : store_(store), port_(port) {}

bool TcpServer::Start() {
  // A local variable throughout setup, not listen_fd_ directly: Start()
  // only needs to *write* listen_fd_ once, right at the end, after every
  // step has already succeeded — using a plain local for everything up
  // to that point means there's no reason to ever read the atomic back
  // during setup, and no failure path needs to remember to reset it
  // (it's simply never written to on failure, and stays at its initial
  // -1).
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return false;
  }

  // SO_REUSEADDR: without this, restarting the server shortly after a
  // previous run (including after Stop()) can fail to bind with
  // "Address already in use" while the OS still holds the old socket in
  // TIME_WAIT — a routine development annoyance, not a correctness
  // issue, that every real server sets this to avoid.
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(static_cast<std::uint16_t>(port_));

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::perror("bind");
    ::close(fd);
    return false;
  }

  if (::listen(fd, kListenBacklog) < 0) {
    std::perror("listen");
    ::close(fd);
    return false;
  }

  listen_fd_.store(fd, std::memory_order_relaxed);
  return true;
}

void TcpServer::AcceptLoop() {
  while (!stop_flag_.load(std::memory_order_relaxed)) {
    sockaddr_in client_addr{};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd =
        ::accept(listen_fd_.load(std::memory_order_relaxed), reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) {
      // Stop() closing listen_fd_ from another thread is the expected,
      // routine way this loop ends — not worth logging as an error in
      // that case. (Closing a fd that another thread is blocked in
      // accept() on is a well-known, widely-used pattern for exactly
      // this kind of shutdown; it's a minor POSIX gray area — the fd
      // number could in principle be reused by an unrelated concurrent
      // open() before accept() wakes up — accepted here the same way
      // this project has accepted a few other well-understood, named
      // tradeoffs elsewhere, e.g. HashKey's FNV-1a not being
      // cryptographically secure.)
      if (stop_flag_.load(std::memory_order_relaxed)) {
        break;
      }
      continue;  // a transient accept() error shouldn't take the whole server down.
    }

    {
      std::lock_guard<std::mutex> lock(clients_mu_);
      client_fds_.push_back(client_fd);
    }
    active_clients_.fetch_add(1, std::memory_order_relaxed);
    std::thread(&TcpServer::HandleClient, this, client_fd).detach();
  }
}

void TcpServer::Stop() {
  stop_flag_.store(true, std::memory_order_relaxed);

  int fd = listen_fd_.load(std::memory_order_relaxed);
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    listen_fd_.store(-1, std::memory_order_relaxed);
  }

  // Copy the fd list out from under the lock before calling shutdown()
  // on each one: HandleClient's own cleanup also takes clients_mu_, so
  // holding it across the shutdown() calls below would risk this thread
  // blocking on a lock a client thread needs to release in order to
  // finish and unblock this thread's later drain-wait loop — a
  // lock-ordering headache avoided entirely by not holding the lock
  // longer than it takes to copy the list.
  std::vector<int> fds_to_close;
  {
    std::lock_guard<std::mutex> lock(clients_mu_);
    fds_to_close = client_fds_;
  }
  for (int fd : fds_to_close) {
    // shutdown(), not close(): a client thread may be blocked in recv()
    // on this exact fd right now. shutdown(SHUT_RDWR) is specifically
    // designed to be safe to call concurrently with a blocked recv()/
    // send() on the same fd from another thread — it makes that call
    // return promptly (0 or an error) without invalidating the fd number
    // itself. close() from a different thread than the one using the fd
    // is the genuinely unsafe move here (the fd could be reused by an
    // unrelated open() before the other thread's blocked call notices) —
    // so the owning thread (HandleClient itself) is the one that calls
    // close(), once its own loop exits normally after shutdown() wakes
    // it up.
    ::shutdown(fd, SHUT_RDWR);
  }

  // Bounded wait for detached client threads to notice and finish, so a
  // caller of Stop() (main.cpp: taking a final RDB snapshot right after
  // Stop() returns) doesn't race against a client thread still mid-
  // command against the same Store.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (active_clients_.load(std::memory_order_relaxed) > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void TcpServer::HandleClient(int client_fd) {
  // RAII: however this function exits (client disconnected, protocol
  // error, send failure), the client fd always gets closed and untracked
  // exactly once, with no duplicated cleanup code at every early-return
  // site below.
  struct ClientGuard {
    TcpServer* server;
    int fd;
    ~ClientGuard() {
      ::close(fd);
      {
        std::lock_guard<std::mutex> lock(server->clients_mu_);
        auto& fds = server->client_fds_;
        fds.erase(std::remove(fds.begin(), fds.end(), fd), fds.end());
      }
      server->active_clients_.fetch_sub(1, std::memory_order_relaxed);
    }
  } guard{this, client_fd};

  CommandParser parser;
  std::vector<char> recv_buf(kRecvBufSize);

  while (true) {
    ssize_t n = ::recv(client_fd, recv_buf.data(), recv_buf.size(), 0);
    if (n <= 0) {
      return;  // 0 = peer closed the connection; <0 = socket error (including a forced shutdown() from Stop()).
    }
    parser.Feed(recv_buf.data(), static_cast<std::size_t>(n));

    std::vector<std::string> args;
    std::string parse_error;
    CommandParser::Status status;
    while ((status = parser.TryParseCommand(&args, &parse_error)) == CommandParser::Status::kComplete) {
      RespValue reply = Dispatch(store_, args);
      if (!SendAll(client_fd, reply.Encode())) {
        return;
      }
    }
    if (status == CommandParser::Status::kProtocolError) {
      SendAll(client_fd, RespValue::Error("ERR Protocol error: " + parse_error).Encode());
      return;  // unrecoverable — matches real Redis, which also drops the connection on a protocol violation.
    }
  }
}

}  // namespace goredis

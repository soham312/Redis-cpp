#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "bench_util.h"
#include "server/tcp_server.h"
#include "store/store.h"

namespace goredis::bench {

namespace {

int ConnectToServer(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::string EncodeMultibulk(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) {
    out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  }
  return out;
}

std::string RecvExact(int fd, std::size_t want) {
  std::string buf;
  char tmp[4096];
  while (buf.size() < want) {
    ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0) {
      break;
    }
    buf.append(tmp, static_cast<std::size_t>(n));
  }
  return buf;
}

}  // namespace

void RunServerBenchmarks() {
  using goredis::Store;
  using goredis::TcpServer;

  Section("End-to-end server throughput (real POSIX sockets, single connection, one round trip per op)");

  Store store;
  TcpServer server(store, 0);  // port 0: OS-assigned, avoids colliding with anything else running locally
  if (!server.Start()) {
    std::printf("Failed to start benchmark server — skipping server benchmarks.\n");
    return;
  }
  std::thread accept_thread([&server] { server.AcceptLoop(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  int fd = ConnectToServer(server.ListenPort());
  if (fd < 0) {
    std::printf("Failed to connect to benchmark server — skipping server benchmarks.\n");
    server.Stop();
    accept_thread.join();
    return;
  }

  {
    std::string req = EncodeMultibulk({"PING"});
    Run("PING round trip", 20000, [&] {
      ::send(fd, req.data(), req.size(), 0);
      RecvExact(fd, 7);  // "+PONG\r\n"
    });
  }

  {
    long long i = 0;
    Run("SET round trip", 20000, [&] {
      std::string req = EncodeMultibulk({"SET", "k" + std::to_string(i % 1000), "value"});
      ::send(fd, req.data(), req.size(), 0);
      RecvExact(fd, 5);  // "+OK\r\n"
      ++i;
    });
  }

  {
    // Every key in [0, 1000) was just SET above (with value "value"),
    // so every GET reply below is exactly "$5\r\nvalue\r\n" (11 bytes) —
    // no nil replies to special-case.
    long long i = 0;
    Run("GET round trip", 20000, [&] {
      std::string req = EncodeMultibulk({"GET", "k" + std::to_string(i % 1000)});
      ::send(fd, req.data(), req.size(), 0);
      RecvExact(fd, 11);
      ++i;
    });
  }

  ::close(fd);
  server.Stop();
  accept_thread.join();

  std::printf(
      "\nNote: each op above pays one full network round trip (send, kernel scheduling, recv) — this measures the "
      "whole stack's per-request latency, not raw Store throughput (see the Store benchmarks above for that). A "
      "real client pipelining many requests per round trip would see dramatically higher throughput; that's not "
      "what's being measured here.\n");
}

}  // namespace goredis::bench

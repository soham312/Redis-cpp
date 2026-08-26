#include "server/tcp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "store/store.h"

using goredis::Store;
using goredis::TcpServer;

namespace {

int ConnectToServer(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  // Bounds this test client's recv() calls so a server bug (hanging
  // instead of replying) fails the affected test with a clear timeout
  // rather than hanging the whole suite.
  timeval tv{};
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

std::string RecvAtLeast(int fd, std::size_t want) {
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

class TcpServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // port 0: let the OS assign a free ephemeral port rather than
    // picking one ourselves. Needed because ctest (via
    // gtest_discover_tests) runs each TEST_F as its own separate
    // process — a static "next free port" counter here would reset in
    // every process anyway, so it can't actually coordinate across
    // tests the way it would within a single run of the raw binary.
    // Binding to 0 sidesteps the coordination problem entirely: the OS
    // guarantees whatever port it hands back isn't in use by anything
    // else on the system.
    server_ = std::make_unique<TcpServer>(store_, 0);
    ASSERT_TRUE(server_->Start());
    port_ = server_->ListenPort();
    accept_thread_ = std::thread([this] { server_->AcceptLoop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the accept loop actually start
  }

  void TearDown() override {
    server_->Stop();
    accept_thread_.join();
  }

  Store store_;
  int port_ = 0;
  std::unique_ptr<TcpServer> server_;
  std::thread accept_thread_;
};

TEST_F(TcpServerTest, RespondsToPing) {
  int fd = ConnectToServer(port_);
  ASSERT_GE(fd, 0);
  std::string req = EncodeMultibulk({"PING"});
  ASSERT_EQ(::send(fd, req.data(), req.size(), 0), static_cast<ssize_t>(req.size()));
  EXPECT_EQ(RecvAtLeast(fd, 7), "+PONG\r\n");
  ::close(fd);
}

TEST_F(TcpServerTest, SetAndGetRoundTripOverTheWire) {
  int fd = ConnectToServer(port_);
  ASSERT_GE(fd, 0);
  std::string req = EncodeMultibulk({"SET", "netkey", "netvalue"});
  ::send(fd, req.data(), req.size(), 0);
  EXPECT_EQ(RecvAtLeast(fd, 5), "+OK\r\n");

  req = EncodeMultibulk({"GET", "netkey"});
  ::send(fd, req.data(), req.size(), 0);
  EXPECT_EQ(RecvAtLeast(fd, 14), "$8\r\nnetvalue\r\n");
  ::close(fd);
}

TEST_F(TcpServerTest, HandlesCommandFragmentedAcrossManySmallWrites) {
  int fd = ConnectToServer(port_);
  ASSERT_GE(fd, 0);
  std::string req = EncodeMultibulk({"ECHO", "fragmented"});
  for (char c : req) {
    ::send(fd, &c, 1, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(RecvAtLeast(fd, 18), "$10\r\nfragmented\r\n");
  ::close(fd);
}

TEST_F(TcpServerTest, HandlesPipelinedCommandsInOneWrite) {
  int fd = ConnectToServer(port_);
  ASSERT_GE(fd, 0);
  std::string req = EncodeMultibulk({"SET", "a", "1"}) + EncodeMultibulk({"SET", "b", "2"});
  ::send(fd, req.data(), req.size(), 0);
  EXPECT_EQ(RecvAtLeast(fd, 10), "+OK\r\n+OK\r\n");
  ::close(fd);
}

TEST_F(TcpServerTest, ClosesConnectionAfterProtocolError) {
  int fd = ConnectToServer(port_);
  ASSERT_GE(fd, 0);
  std::string bad = "*2\r\n$abc\r\n";
  ::send(fd, bad.data(), bad.size(), 0);
  std::string reply = RecvAtLeast(fd, 1);
  EXPECT_EQ(reply.rfind("-ERR", 0), 0u);
  char probe;
  EXPECT_LE(::recv(fd, &probe, 1, 0), 0);  // server closed the connection after the protocol error
  ::close(fd);
}

TEST_F(TcpServerTest, WrongTypeErrorSurfacesOverTheWire) {
  int fd = ConnectToServer(port_);
  ASSERT_GE(fd, 0);
  std::string req = EncodeMultibulk({"RPUSH", "alist", "x"});
  ::send(fd, req.data(), req.size(), 0);
  RecvAtLeast(fd, 4);
  req = EncodeMultibulk({"GET", "alist"});
  ::send(fd, req.data(), req.size(), 0);
  EXPECT_EQ(RecvAtLeast(fd, 10).rfind("-WRONGTYPE", 0), 0u);
  ::close(fd);
}

TEST_F(TcpServerTest, InlineCommandWorksOverARealSocket) {
  int fd = ConnectToServer(port_);
  ASSERT_GE(fd, 0);
  std::string req = "PING\r\n";
  ::send(fd, req.data(), req.size(), 0);
  EXPECT_EQ(RecvAtLeast(fd, 7), "+PONG\r\n");
  ::close(fd);
}

TEST_F(TcpServerTest, ManyConcurrentClientsDoNotCorruptSharedState) {
  constexpr int kClients = 8;
  constexpr int kOpsPerClient = 100;
  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; ++t) {
    threads.emplace_back([this, t] {
      int fd = ConnectToServer(port_);
      ASSERT_GE(fd, 0);
      for (int i = 0; i < kOpsPerClient; ++i) {
        std::string key = "ck" + std::to_string(t) + "_" + std::to_string(i % 20);
        std::string req = EncodeMultibulk({"SET", key, "v"});
        ::send(fd, req.data(), req.size(), 0);
        RecvAtLeast(fd, 5);
        req = EncodeMultibulk({"RPUSH", "shared", key});
        ::send(fd, req.data(), req.size(), 0);
        RecvAtLeast(fd, 1);
      }
      ::close(fd);
    });
  }
  for (auto& th : threads) {
    th.join();
  }
}

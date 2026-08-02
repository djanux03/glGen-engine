#include <doctest/doctest.h>

#include "CommandServer.h"

#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using test_socket_t = SOCKET;
#define TEST_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using test_socket_t = int;
#define TEST_CLOSE ::close
#endif

// These drive the real socket path over loopback rather than poking at
// internals: the framing, the JSON-RPC envelope and the deferred-reply
// mechanism are exactly what an MCP client depends on, and none of them are
// exercised by calling handlers directly.

using json = nlohmann::json;
using bridge::CommandServer;
using bridge::Response;

namespace {

// Ports are picked high and per-test-case to avoid collisions with anything
// else on the machine (and with a previous case's lingering TIME_WAIT).
uint16_t nextPort() {
  static uint16_t port = 47820;
  return port++;
}

// A raw client speaking the same newline-delimited JSON the engine expects.
class TestClient {
public:
  explicit TestClient(uint16_t port) {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    mSock = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    mConnected =
        ::connect(mSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
  }
  ~TestClient() {
    if (mSock != static_cast<test_socket_t>(-1))
      TEST_CLOSE(mSock);
#ifdef _WIN32
    WSACleanup();
#endif
  }

  bool connected() const { return mConnected; }

  void sendLine(const std::string &line) {
    const std::string payload = line + "\n";
    ::send(mSock, payload.data(), static_cast<int>(payload.size()), 0);
  }

  // Reads one reply, pumping `server.poll()` meanwhile -- handlers only ever
  // run on the polling thread, which in production is the frame loop and here
  // is this one.
  json readReply(CommandServer &server, int timeoutMs = 4000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      const size_t nl = mBuffer.find('\n');
      if (nl != std::string::npos) {
        const std::string line = mBuffer.substr(0, nl);
        mBuffer.erase(0, nl + 1);
        return json::parse(line);
      }
      server.poll();
      char buf[4096];
#ifdef _WIN32
      u_long available = 0;
      ioctlsocket(mSock, FIONREAD, &available);
      if (available > 0) {
        const int n = ::recv(mSock, buf, sizeof(buf), 0);
        if (n > 0)
          mBuffer.append(buf, static_cast<size_t>(n));
      }
#else
      fd_set set;
      FD_ZERO(&set);
      FD_SET(mSock, &set);
      timeval tv{0, 1000};
      if (::select(mSock + 1, &set, nullptr, nullptr, &tv) > 0) {
        const int n = ::recv(mSock, buf, sizeof(buf), 0);
        if (n > 0)
          mBuffer.append(buf, static_cast<size_t>(n));
      }
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return json();
  }

  // Drains for a while and reports whether anything arrived. Used to prove a
  // notification produces NO reply.
  bool anythingArrives(CommandServer &server, int forMs = 250) {
    const json reply = readReply(server, forMs);
    return !reply.is_null();
  }

private:
  test_socket_t mSock = static_cast<test_socket_t>(-1);
  bool mConnected = false;
  std::string mBuffer;
};

} // namespace

TEST_CASE("CommandServer — round-trips a request over a real socket") {
  CommandServer server;
  server.setHandler("echo", [](const json &params, uint64_t) {
    return Response::ok(json{{"got", params.value("value", 0)}});
  });

  const uint16_t port = nextPort();
  REQUIRE(server.start(port));
  CHECK(server.running());
  CHECK(server.port() == port);

  TestClient client(port);
  REQUIRE(client.connected());
  client.sendLine(R"({"jsonrpc":"2.0","id":1,"method":"echo","params":{"value":42}})");

  const json reply = client.readReply(server);
  REQUIRE(reply.is_object());
  CHECK(reply["id"] == 1);
  CHECK(reply["result"]["got"] == 42);
  server.stop();
  CHECK_FALSE(server.running());
}

TEST_CASE("CommandServer — handlers run only on the polling thread") {
  CommandServer server;
  std::thread::id handlerThread;
  server.setHandler("who", [&](const json &, uint64_t) {
    handlerThread = std::this_thread::get_id();
    return Response::ok();
  });

  const uint16_t port = nextPort();
  REQUIRE(server.start(port));
  TestClient client(port);
  REQUIRE(client.connected());
  client.sendLine(R"({"jsonrpc":"2.0","id":1,"method":"who"})");
  client.readReply(server);

  // The whole safety argument rests on this: Registry/AssetManager/renderer
  // are lock-free, so a handler must never run on the socket thread.
  CHECK(handlerThread == std::this_thread::get_id());
  server.stop();
}

TEST_CASE("CommandServer — errors, unknown methods and bad JSON stay in-band") {
  CommandServer server;
  server.setHandler("boom", [](const json &, uint64_t) {
    return Response::fail("it broke");
  });
  server.setHandler("throws", [](const json &, uint64_t) -> Response {
    throw std::runtime_error("unexpected");
  });

  const uint16_t port = nextPort();
  REQUIRE(server.start(port));
  TestClient client(port);
  REQUIRE(client.connected());

  client.sendLine(R"({"jsonrpc":"2.0","id":1,"method":"boom"})");
  json reply = client.readReply(server);
  REQUIRE(reply.is_object());
  CHECK(reply["error"]["message"] == "it broke");

  client.sendLine(R"({"jsonrpc":"2.0","id":2,"method":"nope"})");
  reply = client.readReply(server);
  REQUIRE(reply.is_object());
  CHECK(std::string(reply["error"]["message"]).find("unknown method") !=
        std::string::npos);

  client.sendLine("{ not json at all");
  reply = client.readReply(server);
  REQUIRE(reply.is_object());
  CHECK(std::string(reply["error"]["message"]).find("parse error") !=
        std::string::npos);

  // A throwing handler must not take the frame loop down with it.
  client.sendLine(R"({"jsonrpc":"2.0","id":4,"method":"throws"})");
  reply = client.readReply(server);
  REQUIRE(reply.is_object());
  CHECK(std::string(reply["error"]["message"]).find("handler threw") !=
        std::string::npos);
  CHECK(server.running());

  server.stop();
}

TEST_CASE("CommandServer — a deferred request replies only when completed") {
  CommandServer server;
  uint64_t captured = 0;
  server.setHandler("slow", [&](const json &, uint64_t token) {
    captured = token;
    return Response::defer();
  });

  const uint16_t port = nextPort();
  REQUIRE(server.start(port));
  TestClient client(port);
  REQUIRE(client.connected());

  client.sendLine(R"({"jsonrpc":"2.0","id":7,"method":"slow"})");

  // Nothing may come back yet -- this is what makes "capture and give me the
  // image" answerable at all.
  CHECK_FALSE(client.anythingArrives(server, 250));
  CHECK(captured != 0);
  CHECK(server.deferredCount() == 1);

  server.complete(captured, json{{"path", "shot.png"}});
  const json reply = client.readReply(server);
  REQUIRE(reply.is_object());
  CHECK(reply["id"] == 7);
  CHECK(reply["result"]["path"] == "shot.png");
  CHECK(server.deferredCount() == 0);

  // Completing twice is a no-op, not a double reply or a crash: the client
  // may already have gone away.
  server.complete(captured, json{{"path", "again.png"}});
  CHECK_FALSE(client.anythingArrives(server, 150));

  server.stop();
}

TEST_CASE("CommandServer — a deferred request can fail") {
  CommandServer server;
  uint64_t captured = 0;
  server.setHandler("slow", [&](const json &, uint64_t token) {
    captured = token;
    return Response::defer();
  });

  const uint16_t port = nextPort();
  REQUIRE(server.start(port));
  TestClient client(port);
  REQUIRE(client.connected());
  client.sendLine(R"({"jsonrpc":"2.0","id":9,"method":"slow"})");
  CHECK_FALSE(client.anythingArrives(server, 150));

  server.complete(captured, nullptr, "the render never happened");
  const json reply = client.readReply(server);
  REQUIRE(reply.is_object());
  CHECK(reply["error"]["message"] == "the render never happened");
  server.stop();
}

TEST_CASE("CommandServer — notifications get no reply") {
  CommandServer server;
  int calls = 0;
  server.setHandler("ping", [&](const json &, uint64_t) {
    ++calls;
    return Response::ok();
  });

  const uint16_t port = nextPort();
  REQUIRE(server.start(port));
  TestClient client(port);
  REQUIRE(client.connected());

  // No "id" field: JSON-RPC says a notification is not answered.
  client.sendLine(R"({"jsonrpc":"2.0","method":"ping"})");
  CHECK_FALSE(client.anythingArrives(server, 250));
  CHECK(calls == 1);

  server.stop();
}

TEST_CASE("CommandServer — several requests in one write are all handled") {
  CommandServer server;
  server.setHandler("n", [](const json &params, uint64_t) {
    return Response::ok(json{{"n", params.value("n", -1)}});
  });

  const uint16_t port = nextPort();
  REQUIRE(server.start(port));
  TestClient client(port);
  REQUIRE(client.connected());

  // Framing must survive several messages arriving in a single TCP segment.
  client.sendLine(R"({"jsonrpc":"2.0","id":1,"method":"n","params":{"n":1}}
{"jsonrpc":"2.0","id":2,"method":"n","params":{"n":2}}
{"jsonrpc":"2.0","id":3,"method":"n","params":{"n":3}})");

  for (int expected = 1; expected <= 3; ++expected) {
    const json reply = client.readReply(server);
    REQUIRE(reply.is_object());
    CHECK(reply["id"] == expected);
    CHECK(reply["result"]["n"] == expected);
  }
  server.stop();
}

TEST_CASE("CommandServer — refuses to start twice on the same port") {
  CommandServer first;
  const uint16_t port = nextPort();
  REQUIRE(first.start(port));

  CommandServer second;
  CHECK_FALSE(second.start(port));
  CHECK_FALSE(second.lastError().empty());
  CHECK_FALSE(second.running());

  first.stop();
}

TEST_CASE("CommandServer — stop is idempotent and safe without a start") {
  CommandServer never;
  never.stop(); // no start: must not hang or crash
  CHECK_FALSE(never.running());

  CommandServer server;
  REQUIRE(server.start(nextPort()));
  server.stop();
  server.stop();
  CHECK_FALSE(server.running());
}

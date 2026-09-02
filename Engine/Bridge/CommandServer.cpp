#include "CommandServer.h"

#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#define CLOSE_SOCKET ::close
#endif

namespace bridge {
namespace {

using json = nlohmann::json;

// One line must not be able to exhaust memory: a client that never sends a
// newline would otherwise grow this buffer without bound.
constexpr size_t kMaxLineBytes = 4 * 1024 * 1024;

struct Connection {
  socket_t sock = kInvalidSocket;
  uint64_t id = 0;
  std::string inBuffer;
  std::string outBuffer;
};

} // namespace

struct CommandServer::Impl {
  socket_t listener = kInvalidSocket;
  std::vector<Connection> connections;
  std::thread thread;
  uint64_t nextConnectionId = 1;
  std::atomic<bool> stopping{false};
#ifdef _WIN32
  bool wsaStarted = false;
#endif
};

CommandServer::CommandServer() : mImpl(std::make_unique<Impl>()) {}

CommandServer::~CommandServer() { stop(); }

void CommandServer::setHandler(const std::string &method, Handler handler) {
  mHandlers[method] = std::move(handler);
}

bool CommandServer::start(uint16_t port) {
  if (mRunning.load())
    return true;

#ifdef _WIN32
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    mLastError = "WSAStartup failed";
    return false;
  }
  mImpl->wsaStarted = true;
#endif

  mImpl->listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (mImpl->listener == kInvalidSocket) {
    mLastError = "socket() failed";
    return false;
  }

  // Windows' SO_REUSEADDR is NOT the POSIX one: it lets a second process bind
  // an address already in use, so setting it here would let anything on the
  // machine hijack the port -- on a channel that runs arbitrary Lua in this
  // process. SO_EXCLUSIVEADDRUSE is the correct Windows spelling of "this port
  // is mine". POSIX keeps SO_REUSEADDR, where it only skips TIME_WAIT.
  int exclusive = 1;
#ifdef _WIN32
  ::setsockopt(mImpl->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
#else
  ::setsockopt(mImpl->listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  // Loopback only, never INADDR_ANY: a client on this port can run arbitrary
  // Lua inside the engine process.
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(mImpl->listener, reinterpret_cast<sockaddr *>(&addr),
             sizeof(addr)) != 0) {
    mLastError = "bind to 127.0.0.1:" + std::to_string(port) +
                 " failed (port already in use?)";
    CLOSE_SOCKET(mImpl->listener);
    mImpl->listener = kInvalidSocket;
    return false;
  }
  if (::listen(mImpl->listener, 4) != 0) {
    mLastError = "listen() failed";
    CLOSE_SOCKET(mImpl->listener);
    mImpl->listener = kInvalidSocket;
    return false;
  }

  mPort = port;
  mRunning.store(true);
  mImpl->thread = std::thread([this] { socketLoop_(); });
  LOG_INFO("Bridge", "Command port listening on 127.0.0.1:" +
                         std::to_string(port));
  return true;
}

void CommandServer::stop() {
  if (!mRunning.exchange(false)) {
    return;
  }
  mImpl->stopping = true;
  // Closing the listener wakes the select() in the socket thread.
  if (mImpl->listener != kInvalidSocket) {
    CLOSE_SOCKET(mImpl->listener);
    mImpl->listener = kInvalidSocket;
  }
  if (mImpl->thread.joinable())
    mImpl->thread.join();
  for (auto &c : mImpl->connections)
    if (c.sock != kInvalidSocket)
      CLOSE_SOCKET(c.sock);
  mImpl->connections.clear();
#ifdef _WIN32
  if (mImpl->wsaStarted) {
    WSACleanup();
    mImpl->wsaStarted = false;
  }
#endif
  LOG_INFO("Bridge", "Command port stopped");
}

void CommandServer::socketLoop_() {
  while (mRunning.load()) {
    socket_t listener = mImpl->listener;
    if (mImpl->stopping) break;

    fd_set readSet;
    fd_set writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);

    socket_t maxFd = 0;
    if (listener != kInvalidSocket) {
      FD_SET(listener, &readSet);
      maxFd = std::max<socket_t>(maxFd, listener);
    }
    for (auto &c : mImpl->connections) {
      FD_SET(c.sock, &readSet);
      if (!c.outBuffer.empty())
        FD_SET(c.sock, &writeSet);
      maxFd = std::max<socket_t>(maxFd, c.sock);
    }

    // Short timeout so responses queued by the main thread go out promptly
    // and the stop flag is noticed even with no traffic.
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 20 * 1000;
    const int ready = ::select(static_cast<int>(maxFd) + 1, &readSet, &writeSet,
                               nullptr, &timeout);
    if (!mRunning.load())
      break;

    // Move anything the main thread produced into per-connection out buffers.
    {
      std::lock_guard<std::mutex> lock(mMutex);
      while (!mOutbox.empty()) {
        Outgoing out = std::move(mOutbox.front());
        mOutbox.pop_front();
        for (auto &c : mImpl->connections) {
          if (c.id == out.connectionId) {
            c.outBuffer += out.line;
            c.outBuffer += '\n';
            break;
          }
        }
      }
    }

    if (ready > 0 && listener != kInvalidSocket &&
        FD_ISSET(listener, &readSet)) {
      const socket_t client = ::accept(listener, nullptr, nullptr);
      if (client != kInvalidSocket) {
        Connection c;
        c.sock = client;
        c.id = mImpl->nextConnectionId++;
        mImpl->connections.push_back(std::move(c));
      }
    }

    for (size_t i = 0; i < mImpl->connections.size();) {
      Connection &c = mImpl->connections[i];
      bool dead = false;

      if (FD_ISSET(c.sock, &readSet)) {
        char buf[8192];
        const int n = ::recv(c.sock, buf, sizeof(buf), 0);
        if (n <= 0) {
          dead = true;
        } else {
          c.inBuffer.append(buf, static_cast<size_t>(n));
          if (c.inBuffer.size() > kMaxLineBytes) {
            LOG_WARN("Bridge", "dropping oversized request line");
            c.inBuffer.clear();
            dead = true;
          }
          size_t nl;
          while (!dead && (nl = c.inBuffer.find('\n')) != std::string::npos) {
            std::string line = c.inBuffer.substr(0, nl);
            c.inBuffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
              line.pop_back();
            if (!line.empty())
              handleLine_(c.id, line);
          }
        }
      }

      if (!dead && !c.outBuffer.empty() && FD_ISSET(c.sock, &writeSet)) {
        const int sent = ::send(c.sock, c.outBuffer.data(),
                                static_cast<int>(c.outBuffer.size()), 0);
        if (sent <= 0)
          dead = true;
        else
          c.outBuffer.erase(0, static_cast<size_t>(sent));
      }

      if (dead) {
        CLOSE_SOCKET(c.sock);
        mImpl->connections.erase(mImpl->connections.begin() +
                                 static_cast<long>(i));
      } else {
        ++i;
      }
    }
  }
}

void CommandServer::handleLine_(uint64_t connectionId, const std::string &line) {
  json parsed;
  try {
    parsed = json::parse(line);
  } catch (const std::exception &e) {
    enqueueResponse_(connectionId, nullptr, nullptr,
                     std::string("parse error: ") + e.what(),
                     /*allowNullId=*/true);
    return;
  }
  if (!parsed.is_object() || !parsed.contains("method") ||
      !parsed["method"].is_string()) {
    enqueueResponse_(connectionId, parsed.value("id", json()), nullptr,
                     "request must be an object with a string 'method'",
                     /*allowNullId=*/true);
    return;
  }

  Request req;
  req.connectionId = connectionId;
  req.id = parsed.value("id", json());
  req.method = parsed["method"].get<std::string>();
  req.params = parsed.contains("params") ? parsed["params"] : json::object();
  req.token = mNextToken.fetch_add(1);

  std::lock_guard<std::mutex> lock(mMutex);
  mInbox.push_back(std::move(req));
}

void CommandServer::enqueueResponse_(uint64_t connectionId, const json &id,
                                     const json &result,
                                     const std::string &error,
                                     bool allowNullId) {
  // A notification (no id) gets no reply, per JSON-RPC -- except for parse
  // errors, where there is no id to echo but the client still needs to know.
  if (id.is_null() && !allowNullId)
    return;
  json response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  if (error.empty())
    response["result"] = result;
  else
    response["error"] = json{{"code", -32000}, {"message", error}};

  std::lock_guard<std::mutex> lock(mMutex);
  mOutbox.push_back(Outgoing{connectionId, response.dump()});
}

void CommandServer::poll() {
  std::deque<Request> batch;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    batch.swap(mInbox);
  }

  for (const Request &req : batch) {
    const auto it = mHandlers.find(req.method);
    if (it == mHandlers.end()) {
      enqueueResponse_(req.connectionId, req.id, nullptr,
                       "unknown method '" + req.method + "'");
      continue;
    }

    Response response;
    try {
      response = it->second(req.params, req.token);
    } catch (const std::exception &e) {
      // A throwing handler must not take the frame loop down with it.
      response = Response::fail(std::string("handler threw: ") + e.what());
    }

    if (response.deferred) {
      if (req.id.is_null()) {
        // Nothing to defer to -- a notification has no reply channel.
        continue;
      }
      std::lock_guard<std::mutex> lock(mMutex);
      mDeferred[req.token] = {req.connectionId, req.id};
      continue;
    }
    enqueueResponse_(req.connectionId, req.id, response.result, response.error);
  }
}

void CommandServer::complete(uint64_t token, json result, std::string error) {
  uint64_t connectionId = 0;
  json id;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    const auto it = mDeferred.find(token);
    if (it == mDeferred.end())
      return; // already completed, or the client vanished
    connectionId = it->second.first;
    id = it->second.second;
    mDeferred.erase(it);
  }
  enqueueResponse_(connectionId, id, result, error);
}

size_t CommandServer::deferredCount() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mDeferred.size();
}

} // namespace bridge

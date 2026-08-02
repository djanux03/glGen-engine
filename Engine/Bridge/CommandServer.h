#pragma once
// CommandServer.h — a localhost JSON-RPC 2.0 port into the running engine.
//
// This is the transport half of AI_ASSET_PIPELINE_PLAN.md Phase 3: the MCP
// server (Phase 4) talks to it over TCP, but so can a shell script, a CI job,
// or a test. It is deliberately engine-agnostic -- it knows about sockets,
// framing and queues, and nothing about meshes.
//
// THE THREADING RULE (§3.2). Registry, AssetManager, Scene and VulkanRenderer
// are single-threaded with no locking. So:
//
//   * the socket thread ONLY accepts, reads, frames and parses JSON, then
//     pushes a Request onto a mutex-guarded queue;
//   * poll() runs on the MAIN THREAD at one fixed point in the frame and is
//     the only place a handler ever executes;
//   * responses go back through a second queue for the socket thread to write.
//
// Handlers therefore never need a lock and can touch engine state freely.
//
// DEFERRED REPLIES. Some work cannot answer within one frame -- a screenshot
// is written by the NEXT drawFrame, and a turntable spans many. Such a handler
// returns Response::defer() and the request stays open until someone calls
// complete(token, ...). This is why the protocol carries an id at all: without
// deferred completion, "capture and give me the image" is unanswerable.
//
// SECURITY. Binds to 127.0.0.1 only, and is off unless a port is passed. Any
// client that can connect can run arbitrary Lua in the engine process, so this
// is a development tool, not something to expose.

#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

// What a handler gives back. Exactly one of: a result, an error, or a
// deferral.
struct Response {
  nlohmann::json result;
  std::string error;
  bool deferred = false;

  static Response ok(nlohmann::json value = nlohmann::json::object()) {
    Response r;
    r.result = std::move(value);
    return r;
  }
  static Response fail(std::string message) {
    Response r;
    r.error = std::move(message);
    return r;
  }
  // The handler has taken ownership of the reply and will call
  // CommandServer::complete() with its token later.
  static Response defer() {
    Response r;
    r.deferred = true;
    return r;
  }
};

class CommandServer {
public:
  // `token` identifies this request for a deferred completion; ignore it for
  // handlers that answer immediately.
  using Handler =
      std::function<Response(const nlohmann::json &params, uint64_t token)>;

  CommandServer();
  ~CommandServer();
  CommandServer(const CommandServer &) = delete;
  CommandServer &operator=(const CommandServer &) = delete;

  // Binds 127.0.0.1:port and starts the socket thread. Returns false (with a
  // message in lastError()) if the port is taken or sockets are unavailable.
  bool start(uint16_t port);
  void stop();
  bool running() const { return mRunning.load(); }
  uint16_t port() const { return mPort; }
  const std::string &lastError() const { return mLastError; }

  // Must be called before start() (or at least before a client connects).
  void setHandler(const std::string &method, Handler handler);

  // MAIN THREAD ONLY. Executes every queued request and hands finished
  // responses to the socket thread. Call once per frame at a fixed point.
  void poll();

  // MAIN THREAD ONLY. Finishes a request whose handler returned defer().
  // Completing an unknown token is a no-op (the client may have disconnected).
  void complete(uint64_t token, nlohmann::json result, std::string error = {});

  // Requests handed to handlers that have not been completed yet.
  size_t deferredCount() const;

private:
  struct Request {
    uint64_t connectionId = 0;
    nlohmann::json id;   // JSON-RPC id; null for notifications
    std::string method;
    nlohmann::json params;
    uint64_t token = 0;
  };
  struct Outgoing {
    uint64_t connectionId = 0;
    std::string line; // already-serialized JSON, newline appended by the writer
  };

  void socketLoop_();
  void handleLine_(uint64_t connectionId, const std::string &line);
  // A null id normally means "notification", which JSON-RPC says gets no
  // reply. `allowNullId` is the documented exception: a request whose JSON
  // failed to parse has no recoverable id, and swallowing that would leave a
  // client waiting forever on a typo.
  void enqueueResponse_(uint64_t connectionId, const nlohmann::json &id,
                        const nlohmann::json &result, const std::string &error,
                        bool allowNullId = false);

  // Platform socket state lives behind a pimpl so <winsock2.h> never leaks
  // into anything that includes this header.
  struct Impl;
  std::unique_ptr<Impl> mImpl;

  std::unordered_map<std::string, Handler> mHandlers;

  mutable std::mutex mMutex;
  std::deque<Request> mInbox;    // socket thread -> main thread
  std::deque<Outgoing> mOutbox;  // main thread -> socket thread
  // Open deferred requests, keyed by token, so complete() knows where to send.
  std::unordered_map<uint64_t, std::pair<uint64_t, nlohmann::json>> mDeferred;

  std::atomic<bool> mRunning{false};
  std::atomic<uint64_t> mNextToken{1};
  uint16_t mPort = 0;
  std::string mLastError;
};

} // namespace bridge

#pragma once

#include <cpr/cpr.h>
#include <future>
#include <optional>
#include <string>

struct AppState;

class NetworkSubsystem {
public:
  void init();
  void update(float dt, AppState &state);
  void shutdown();

private:
  float pollTimer = 0.0f;
  float pollInterval = 3.0f; // Seconds between requests

  bool isRequestPending = false;
  std::optional<cpr::AsyncResponse> futureResponse;

  void handleResponse(const cpr::Response &r, AppState &state);
};

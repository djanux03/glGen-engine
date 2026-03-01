#include "NetworkSubsystem.h"
#include "AppState.h"
#include <iostream>
#include <json.hpp>
#include <string>

using json = nlohmann::json;

void NetworkSubsystem::init() {
  std::cout << "[Network] Laravel Subsystem Initialized" << std::endl;
}

void NetworkSubsystem::update(float dt, AppState &state) {
  // If we are already waiting for a response, check if it's done
  if (isRequestPending && futureResponse.has_value()) {
    if (futureResponse.value().wait_for(std::chrono::seconds(0)) ==
        std::future_status::ready) {
      handleResponse(futureResponse.value().get(), state);
      isRequestPending = false;
      futureResponse.reset();
      pollTimer = 0.0f; // Reset timer after success
    }
    return; // Don't tick the timer while fetching
  }

  // Tick the poll timer
  pollTimer += dt;
  if (pollTimer >= pollInterval) {
    // Fire off asynchronous GET request
    // Ensure you have `php artisan serve` running at 8000
    futureResponse =
        cpr::GetAsync(cpr::Url{"http://localhost:8000/api/environment"},
                      cpr::Timeout{2000} // 2 second timeout
        );
    isRequestPending = true;
  }
}

void NetworkSubsystem::handleResponse(const cpr::Response &r, AppState &state) {
  if (r.status_code != 200) {
    // Optionally log connection failures
    // std::cout << "[Network] Failed to contact Laravel API. Code: " <<
    // r.status_code << std::endl;
    return;
  }

  try {
    auto j = json::parse(r.text);

    // Weather/Lighting overrides
    if (j.contains("disableClouds")) {
      state.render.disableClouds = j["disableClouds"].get<bool>();
    }
    if (j.contains("disableHDR")) {
      state.render.disableHDR = j["disableHDR"].get<bool>();
    }
    if (j.contains("ambientStrength")) {
      state.sun.ambientStrength = j["ambientStrength"].get<float>();
    }
    if (j.contains("hasFire")) {
      state.hasFire = j["hasFire"].get<bool>();
    }

    // Remote Spawning
    // Example: Laravel sends { "spawn_drop": true, "spawn_item":
    // "__primitive_cube" }
    if (j.contains("spawn_drop") && j["spawn_drop"].get<bool>() == true) {
      if (j.contains("spawn_item")) {
        std::string item = j["spawn_item"].get<std::string>();
        state.pending.pendingSpawnPaths.push_back(item);
      }
    }

  } catch (const json::parse_error &e) {
    std::cout << "[Network] Failed to parse Laravel JSON: " << e.what()
              << std::endl;
  }
}

void NetworkSubsystem::shutdown() {
  if (isRequestPending && futureResponse.has_value()) {
    // Wait for the final request to complete to prevent thread abort
    futureResponse.value().wait();
  }
}

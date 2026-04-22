#pragma once

#include <cstdint>

struct AppState;

class PlayerControllerSystem {
public:
  void reset();
  void update(AppState &state, float dt);

private:
  float mVerticalVelocity = 0.0f;
  bool mGrounded = false;
  uint32_t mLastPlayerId = 0;
};

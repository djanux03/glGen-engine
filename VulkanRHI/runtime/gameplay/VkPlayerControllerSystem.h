#pragma once

#include <cstdint>

struct VkAppState;

class VkPlayerControllerSystem {
public:
  void reset();
  void update(VkAppState &state, float dt);

private:
  float mVerticalVelocity = 0.0f;
  bool mGrounded = false;
  uint32_t mLastPlayerId = 0;
};

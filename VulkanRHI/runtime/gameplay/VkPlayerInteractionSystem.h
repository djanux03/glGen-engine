#pragma once

struct VkAppState;

class VkPlayerInteractionSystem {
public:
  void reset();
  void update(VkAppState &state, float dt);

private:
  float mCraterCooldown = 0.0f;
};

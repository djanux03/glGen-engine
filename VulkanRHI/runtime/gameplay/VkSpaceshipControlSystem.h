#pragma once

struct VkAppState;

class VkSpaceshipControlSystem {
public:
  void reset();
  void update(VkAppState &state, float dt);
};

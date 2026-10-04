#pragma once
#include <glm/glm.hpp>

struct VkAppState;

class VkPlayerInteractionSystem {
public:
  // Reuses precise physics and authored collider fallbacks for firearm hits.
  void fireRifle(VkAppState &state, const glm::vec3 &origin, const glm::vec3 &direction);
  void reset();
  void update(VkAppState &state, float dt);

private:
  float mCraterCooldown = 0.0f;
};

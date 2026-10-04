#pragma once

#include <cstdint>

struct VkAppState;

class VkPlayerControllerSystem {
public:
  void reset();
  bool grounded() const { return mGrounded; }
  // `active` gates mouse-look/movement -- false while in editor mode (or
  // paused), so the player entity doesn't drift from stray mouse motion
  // over the viewport. Ground/camera-component bookkeeping still runs so
  // the player's state stays coherent for when play mode resumes.
  void update(VkAppState &state, float dt, bool active);

private:
  float mVerticalVelocity = 0.0f;
  bool mGrounded = false;
  uint32_t mLastPlayerId = 0;
};

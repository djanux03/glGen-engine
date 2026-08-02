#pragma once

struct VkAppState;

// Per-frame gameplay update order for glGenVk, replicating the old
// CoreAppLayer::update sequence adapted to VkAppState. Not an
// IEngineSubsystem -- it has no init/shutdown semantics, just a per-frame
// call from main()'s loop. Stateless/static since all real per-frame state
// lives in VkAppState and the gameplay system instances it owns.
class VkCoreAppLayer {
public:
  static void update(VkAppState &state, float dt, bool simulate);
};

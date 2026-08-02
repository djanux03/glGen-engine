#include "VkCoreAppLayer.h"

#include "../VkAppState.h"

void VkCoreAppLayer::update(VkAppState &state, float dt, bool simulate) {
  state.playerController.update(state, dt);
  state.spaceshipControl.update(state, dt);
  state.scriptSystem.update(state.scene.registry(), dt);
  if (simulate)
    state.physicsSystem.update(state.scene.registry(), dt);
  state.playerInteraction.update(state, dt);
}

#include "VkPhysicsSubsystem.h"

#include "../VkAppState.h"

bool VkPhysicsSubsystem::initialize() {
  mState.physicsSystem.init();
  return true;
}

void VkPhysicsSubsystem::shutdown() { mState.physicsSystem.shutdown(); }

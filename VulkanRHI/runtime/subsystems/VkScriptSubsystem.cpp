#include "VkScriptSubsystem.h"

#include "../VkAppState.h"

bool VkScriptSubsystem::initialize() {
  mState.scriptSystem.initialize(mState.scene.registry(),
                                 &mState.physicsSystem);
  return true;
}

void VkScriptSubsystem::shutdown() { mState.scriptSystem.shutdown(); }

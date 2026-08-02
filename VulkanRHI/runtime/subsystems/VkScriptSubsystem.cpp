#include "VkScriptSubsystem.h"

#include "../VkAppState.h"
#include "../VkScriptBindings.h"

bool VkScriptSubsystem::initialize() {
  // render.* and terrain.* are registered through a hook: they need renderer
  // and terrain types EngineCore cannot see. Must be added BEFORE initialize()
  // -- hooks run during it.
  VkAppState *state = &mState;
  mState.scriptSystem.addBindingHook(
      [state](sol::state &lua) { registerVkScriptBindings(lua, *state); });

  mState.scriptSystem.initialize(mState.scene.registry(), &mState.physicsSystem,
                                 &mState.scene, &mState.assetLibrary);
  return true;
}

void VkScriptSubsystem::shutdown() { mState.scriptSystem.shutdown(); }

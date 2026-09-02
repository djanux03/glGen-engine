#include "VkScriptSubsystem.h"

#include "../VkAppState.h"
#include "../VkScriptBindings.h"
#include "VkTerrainSubsystem.h"

bool VkScriptSubsystem::initialize() {
  // render.* and terrain.* are registered through a hook: they need renderer
  // and terrain types EngineCore cannot see. Must be added BEFORE initialize()
  // -- hooks run during it.
  VkAppState *state = &mState;
  mState.scriptSystem.addBindingHook(
      [state](sol::state &lua) { registerVkScriptBindings(lua, *state); });

  mState.scriptSystem.initialize(
      mState.scene.registry(), &mState.physicsSystem, &mState.scene,
      &mState.assetLibrary,
      // Resolved through VkAppState each call rather than captured up front:
      // terrain can be created and destroyed at any time, so a pointer taken
      // now would be stale by the time a script uses it.
      [state](float x, float z) -> float {
        return (state->terrainSubsystem && state->terrainSubsystem->hasTerrain())
                   ? state->terrainSubsystem->heightAt(glm::vec2(x, z))
                   : 0.0f;
      });
  return true;
}

void VkScriptSubsystem::shutdown() { mState.scriptSystem.shutdown(); }

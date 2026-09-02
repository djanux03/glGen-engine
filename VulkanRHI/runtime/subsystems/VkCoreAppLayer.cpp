#include "VkCoreAppLayer.h"

#include "../VkAppState.h"
#include "VkTerrainSubsystem.h" // hasTerrain() gates streaming
#include "VulkanRenderer.h"

void VkCoreAppLayer::update(VkAppState &state, float dt, bool simulate,
                             bool playerActive) {
  state.playerController.update(state, dt, playerActive);
  // Spaceship (arrow keys) and interactions (LMB axe swing, Shift+LMB grab)
  // are gameplay input like the player controller: play mode only. In
  // editor mode a viewport click must select/paint, never chop a tree.
  if (playerActive)
    state.spaceshipControl.update(state, dt);
  // Lua gameplay scripts run in play mode only (playState is what the old
  // app gated script execution on): the editor-mode world stays inert.
  if (state.playState == VkAppState::PlayState::Playing)
    state.scriptSystem.update(state.scene.registry(), dt);
  if (simulate)
    state.physicsSystem.update(state.scene.registry(), dt);
  if (playerActive)
    state.playerInteraction.update(state, dt);

  // Not gated by `simulate` -- streaming tracks whichever camera is driving
  // the view right now (free-fly in editor mode, player in play mode; see
  // main.cpp's camPos sync), not physics simulation.
  //
  // Gated on terrain EXISTING, though: the engine boots with none, and
  // streaming an uninitialized TerrainChunkManager would spin up worker
  // threads to build chunks for a world that was never created.
  if (state.renderer && state.terrainSubsystem &&
      state.terrainSubsystem->hasTerrain())
    state.terrain.streamUpdate(state.renderer->params().camPos);
}

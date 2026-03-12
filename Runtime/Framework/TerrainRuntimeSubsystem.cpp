#include "TerrainRuntimeSubsystem.h"

#include "AppState.h"

bool TerrainRuntimeSubsystem::initialize() {
  mState.terrainSystem.init(mState.terrainSettings, mState.scene, &mState.assets);
  mState.terrainSystem.setPhysicsSystem(&mState.physicsSystem);
  return true;
}

void TerrainRuntimeSubsystem::shutdown() { mState.terrainSystem.shutdown(); }

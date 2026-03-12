#include "ScriptRuntimeSubsystem.h"

#include "AppState.h"

bool ScriptRuntimeSubsystem::initialize() {
  mState.scriptSystem.initialize(mState.scene.registry(), &mState.physicsSystem);
  return true;
}

void ScriptRuntimeSubsystem::shutdown() { mState.scriptSystem.shutdown(); }

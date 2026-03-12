#include "PhysicsRuntimeSubsystem.h"

#include "AppState.h"

bool PhysicsRuntimeSubsystem::initialize() {
  mState.physicsSystem.init();
  return true;
}

void PhysicsRuntimeSubsystem::shutdown() { mState.physicsSystem.shutdown(); }

#include "NetworkRuntimeSubsystem.h"

#include "AppState.h"

bool NetworkRuntimeSubsystem::initialize() {
  mState.networkSystem.init();
  return true;
}

void NetworkRuntimeSubsystem::shutdown() { mState.networkSystem.shutdown(); }

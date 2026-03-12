#pragma once

#include "IEngineSubsystem.h"

struct AppState;

class PhysicsRuntimeSubsystem final : public IEngineSubsystem {
public:
  explicit PhysicsRuntimeSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "PhysicsRuntimeSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override {
    return {"RuntimeSystems"};
  }

  bool initialize() override;
  void shutdown() override;

private:
  AppState &mState;
};

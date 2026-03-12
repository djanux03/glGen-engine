#pragma once

#include "IEngineSubsystem.h"

struct AppState;

class TerrainRuntimeSubsystem final : public IEngineSubsystem {
public:
  explicit TerrainRuntimeSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "TerrainRuntimeSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override {
    return {"RuntimeSystems", "PhysicsRuntimeSubsystem"};
  }

  bool initialize() override;
  void shutdown() override;

private:
  AppState &mState;
};

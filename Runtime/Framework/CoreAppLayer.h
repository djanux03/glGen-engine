#pragma once

#include "IEngineSubsystem.h"
#include <string>
#include <vector>

struct AppState;

class CoreAppLayer : public IEngineSubsystem {
public:
  CoreAppLayer(AppState &state) : mState(state) {}

  std::string name() const override { return "CoreAppLayer"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Application; }
  std::vector<std::string> dependencies() const override {
    return {"Window", "AudioSubsystem", "RuntimeSystems", "EditorSubsystem",
            "RenderLoopSubsystem", "PhysicsRuntimeSubsystem",
            "ScriptRuntimeSubsystem", "NetworkRuntimeSubsystem",
            "TerrainRuntimeSubsystem"};
  }

  bool initialize() override;
  void shutdown() override;

  void update(float dt, float nowT);

private:
  void applyHistorySnapshot(int idx);
  void commitHistorySnapshot(const std::string &label);

  AppState &mState;
};

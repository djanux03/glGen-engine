#pragma once

#include "IEngineSubsystem.h"

struct AppState;

class ScriptRuntimeSubsystem final : public IEngineSubsystem {
public:
  explicit ScriptRuntimeSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "ScriptRuntimeSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override {
    return {"RuntimeSystems", "PhysicsRuntimeSubsystem"};
  }

  bool initialize() override;
  void shutdown() override;

private:
  AppState &mState;
};

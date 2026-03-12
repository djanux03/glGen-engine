#pragma once

#include "IEngineSubsystem.h"

struct AppState;

class NetworkRuntimeSubsystem final : public IEngineSubsystem {
public:
  explicit NetworkRuntimeSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "NetworkRuntimeSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override {
    return {"RuntimeSystems"};
  }

  bool initialize() override;
  void shutdown() override;

private:
  AppState &mState;
};

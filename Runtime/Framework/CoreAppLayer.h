#pragma once

#include "IEngineSubsystem.h"
#include <string>
#include <vector>

struct AppState;

class CoreAppLayer : public IEngineSubsystem {
public:
  CoreAppLayer(AppState &state) : mState(state) {}

  std::string name() const override { return "CoreAppLayer"; }
  std::vector<std::string> dependencies() const override {
    return {"Window", "RuntimeSystems", "EditorSubsystem",
            "RenderLoopSubsystem"};
  }

  bool initialize() override;
  void shutdown() override;

  void update(float dt, float nowT);

private:
  void applyHistorySnapshot(int idx);
  void commitHistorySnapshot(const std::string &label);

  AppState &mState;
};

#pragma once

#include "../../../Engine/Core/IEngineSubsystem.h"

struct VkAppState;

// Vulkan-side counterpart of ScriptRuntimeSubsystem: owns init/shutdown of
// VkAppState::scriptSystem, wired to the shared registry and physics system.
class VkScriptSubsystem final : public IEngineSubsystem {
public:
  explicit VkScriptSubsystem(VkAppState &state) : mState(state) {}

  std::string name() const override { return "VkScriptSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override {
    return {"VkPhysicsSubsystem"};
  }

  bool initialize() override;
  void shutdown() override;

private:
  VkAppState &mState;
};

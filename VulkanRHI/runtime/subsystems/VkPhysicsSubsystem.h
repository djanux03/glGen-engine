#pragma once

#include "../../../Engine/Core/IEngineSubsystem.h"

struct VkAppState;

// Vulkan-side counterpart of PhysicsRuntimeSubsystem: owns init/shutdown of
// VkAppState::physicsSystem. No "RuntimeSystems" dependency here -- glGenVk
// has no GL-renderer-init subsystem to depend on; the Vulkan renderer is
// already initialized by main() before subsystems run.
class VkPhysicsSubsystem final : public IEngineSubsystem {
public:
  explicit VkPhysicsSubsystem(VkAppState &state) : mState(state) {}

  std::string name() const override { return "VkPhysicsSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override { return {}; }

  bool initialize() override;
  void shutdown() override;

private:
  VkAppState &mState;
};

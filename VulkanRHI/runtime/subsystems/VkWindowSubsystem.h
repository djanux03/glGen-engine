#pragma once

#include "../../../Engine/Core/IEngineSubsystem.h"

struct VkAppState;

// Platform-phase placeholder: the GLFW window + VulkanRenderer are already
// constructed and wired into VkAppState by main() before the
// SubsystemManager runs. This subsystem does no init/teardown of its own --
// it exists purely so later subsystems can depend on the name "Window" and
// have the dependency graph resolve, matching the old GL app's shape.
class VkWindowSubsystem final : public IEngineSubsystem {
public:
  explicit VkWindowSubsystem(VkAppState &state) : mState(state) {}

  std::string name() const override { return "Window"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Platform; }
  std::vector<std::string> dependencies() const override { return {}; }

  bool initialize() override;
  void shutdown() override;

private:
  VkAppState &mState;
};

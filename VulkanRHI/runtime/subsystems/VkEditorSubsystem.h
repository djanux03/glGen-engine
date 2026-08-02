#pragma once

#include "../../../Engine/Core/IEngineSubsystem.h"

struct VkAppState;

// Tooling-phase placeholder mirroring VkWindowSubsystem's pattern: VkEditor
// is constructed by main() (a local VkEditor editor;) and wired into
// VkAppState::vkEditor before the SubsystemManager runs. This subsystem does
// no construction/teardown of its own -- it just validates the wiring and
// participates in the dependency graph.
class VkEditorSubsystem final : public IEngineSubsystem {
public:
  explicit VkEditorSubsystem(VkAppState &state) : mState(state) {}

  std::string name() const override { return "VkEditorSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Tooling; }
  std::vector<std::string> dependencies() const override {
    return {"Window"};
  }

  bool initialize() override;
  void shutdown() override;

private:
  VkAppState &mState;
};

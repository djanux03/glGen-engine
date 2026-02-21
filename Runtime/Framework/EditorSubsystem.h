#pragma once

#include "IEngineSubsystem.h"
#include <string>
#include <vector>

struct AppState;

class EditorSubsystem final : public IEngineSubsystem {
public:
  explicit EditorSubsystem(AppState &state);
  ~EditorSubsystem() override;

  std::string name() const override { return "EditorSubsystem"; }
  std::vector<std::string> dependencies() const override { return {"Window"}; }

  bool initialize() override;
  void shutdown() override;

  void beginFrame();
  void endFrame();
  void drawDockspace();

private:
  AppState &mState;
};

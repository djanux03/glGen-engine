#pragma once

#include "IEngineSubsystem.h"
#include "EditorState.h"
#include <string>
#include <vector>

struct AppState;

class EditorSubsystem final : public IEngineSubsystem {
public:
  explicit EditorSubsystem(AppState &state);
  ~EditorSubsystem() override;

  std::string name() const override { return "EditorSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Tooling; }
  std::vector<std::string> dependencies() const override { return {"Window"}; }

  bool initialize() override;
  void shutdown() override;

  void beginFrame();
  void endFrame();
  void drawDockspace();

  SelectionState &selection() { return mSelection; }
  HistoryState &history() { return mHistory; }
  PendingActions &pending() { return mPending; }

private:
  AppState &mState;
  SelectionState mSelection;
  HistoryState mHistory;
  PendingActions mPending;
};

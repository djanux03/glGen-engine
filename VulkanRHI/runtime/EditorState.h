#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <string>

struct SelectionState {
  uint32_t selectedEntityId = 0;
  std::vector<uint32_t> selectedEntities;
  uint32_t lastClickedEntity = 0;

  bool editObjPart = false;
  std::string selectedObjPartName;

  bool editColliderBounds =
      false; // Toggle to intercept gizmo scaling for colliders

  // ImGuizmo::OPERATION and MODE stored as int to avoid pulling ImGuizmo
  // into lightweight translation units. Cast when needed.
  int gizmoOp  = 0; // ImGuizmo::TRANSLATE = 0
  int gizmoMode = 0; // ImGuizmo::WORLD = 0

  bool renaming = false;
  char renameBuf[128] = "";
  char outlinerFilter[128] = "";
  float focusDistance = 12.0f;
};

struct PendingActions {
  std::vector<std::string> pendingDropPaths;
  std::vector<std::string> pendingSpawnPaths;
  std::vector<uint32_t> pendingDeleteEntityIds;
  std::vector<std::string> pendingEmptyEntityNames;
  std::vector<std::string> pendingConsoleCommands;
  bool requestTestFootstepAudio = false;

  bool requestSaveConfig = false;
  bool requestLoadConfig = false;
  bool requestSaveProjectConfig = false;
  bool requestSaveProjectDefaults = false;
  bool requestResetProjectDefaults = false;
  std::string pendingSceneSavePath;
  std::string pendingSceneLoadPath;
};

struct HistoryState {
  bool requestUndo = false;
  bool requestRedo = false;
  int requestHistoryJump = -1;
  std::vector<std::string> historySnapshots;
  std::vector<std::string> historyLabels;
  int historyCursor = -1;
  bool pendingHistoryCommit = false;
  std::string pendingHistoryLabel;
};

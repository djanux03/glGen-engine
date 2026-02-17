#pragma once

#include <cstdint>
#include <string>

struct SaveConfigRequestedEvent {};
struct LoadConfigRequestedEvent {};
struct SaveProjectConfigRequestedEvent {};
struct SaveSceneRequestedEvent {
  std::string path;
};
struct LoadSceneRequestedEvent {
  std::string path;
};
struct UndoRequestedEvent {};
struct RedoRequestedEvent {};
struct SceneHistoryJumpRequestedEvent {
  int index = -1;
};

struct SpawnEntityRequestedEvent {
  std::string path;
};

struct CreateEmptyEntityRequestedEvent {
  std::string name;
};

struct DeleteEntityRequestedEvent {
  uint32_t entityId = 0;
};

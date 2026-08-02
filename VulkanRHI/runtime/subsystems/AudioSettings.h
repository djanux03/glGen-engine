#pragma once

#include <string>

struct AudioSettings {
  bool enabled = true;
  bool mute = false;
  float masterVolume = 1.0f;

  bool ambientEnabled = true;
  std::string ambientPath;
  float ambientVolume = 0.65f;

  bool footstepsEnabled = true;
  std::string footstepPath;
  float footstepVolume = 0.60f;
  float footstepWalkCadence = 0.34f;
  float footstepRunCadence = 0.24f;
};

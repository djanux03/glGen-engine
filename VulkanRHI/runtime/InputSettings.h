#pragma once

struct InputSettings {
  float walkStep = 0.03f;
  float runMult = 2.0f;
  float jumpStrength = 0.18f;
  float gravity = 0.01f;
  bool freezePhysics = false;
  bool creativeFlight = false;
  float mouseSensitivity = 0.10f;
  float fov = 50.0f;
};

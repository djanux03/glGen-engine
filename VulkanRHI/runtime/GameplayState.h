#pragma once

#include <cstdint>
#include <string>
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// GameplayState — player, viewmodel, grab, hotbar, and debug gameplay data.
// Previously scattered across AppState as individual fields.
// Owned by CoreAppLayer; editor accesses via reference.
// ---------------------------------------------------------------------------

struct ViewmodelSettings {
  bool axeEnabled = true;
  glm::vec3 axeOffset = glm::vec3(0.06f, -0.15f, 0.24f);
  glm::vec3 axeRotation = glm::vec3(117.5f, 84.5f, 2.0f); // degrees
  glm::vec3 axeScale = glm::vec3(0.66f);
  std::string axePath = "assets/playerassets/axe.obj";

  bool torchEnabled = true;
  glm::vec3 torchOffset = glm::vec3(0.12f, -0.3f, 0.40f);
  glm::vec3 torchRotation = glm::vec3(-20.0f, -20.0f, 0.0f);
  glm::vec3 torchScale = glm::vec3(0.05f, 0.5f, 0.05f);

  bool usePlayerCameraInEdit = true;
};

struct GrabState {
  uint32_t grabbedEntityId = 0;
  float grabbedDistance = 3.0f;
  bool grabbedHadRigidbody = false;
  int grabbedPrevBodyType = 0;
  bool grabbedIsTreeInstance = false;
  std::string grabbedPrefab;
  uint32_t grabbedInstanceIndex = 0;
  glm::mat4 grabbedBaseMatrix = glm::mat4(1.0f);
  glm::vec3 grabbedOffset = glm::vec3(0.0f);
  glm::vec3 grabbedReleaseVelocity = glm::vec3(0.0f);
  bool grabbedReleased = false;
};

struct DebugGameplayOverlay {
  // Camera / mouse debug
  float debugMouseDX = 0.0f;
  float debugMouseDY = 0.0f;
  float debugYaw = 0.0f;
  float debugPitch = 0.0f;
  glm::vec3 debugCamFront = glm::vec3(0.0f, 0.0f, -1.0f);
  glm::vec3 debugCamUp = glm::vec3(0.0f, 1.0f, 0.0f);

  // Gameplay hit debug
  bool debugGameplayHit = false;
  uint32_t debugGameplayHitId = 0;
  float debugGameplayHitDist = 0.0f;
  std::string debugGameplayHitName;
  std::string debugGameplayHitKind;
  std::string debugGameplayMissReason;
  glm::vec3 debugGameplayAimOrigin = glm::vec3(0.0f);
  glm::vec3 debugGameplayAimDirection = glm::vec3(0.0f, 0.0f, -1.0f);
  glm::vec3 debugGameplayHitPosition = glm::vec3(0.0f);

  // Grab debug
  uint32_t debugGrabHitId = 0;
  float debugGrabHitDist = 0.0f;
  std::string debugGrabHitName;
  std::string debugGrabPrefab;
  int debugGrabInstance = -1;
  bool debugGrabMoved = false;
};

struct GameplayState {
  // Player
  uint32_t playerId = 0;
  int woodCount = 0;
  uint32_t axeEntity = 0;
  float lastPlayerYaw = 0.0f;
  float lastPlayerPitch = 0.0f;
  bool hasLastPlayerRot = false;

  // Hotbar
  enum class HotbarSlot { Axe = 1, Torch = 2 };
  HotbarSlot activeSlot = HotbarSlot::Axe;
  uint32_t torchEntity = 0;

  // Sub-structs
  ViewmodelSettings viewmodel;
  GrabState grab;
  DebugGameplayOverlay debug;
};

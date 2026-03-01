#pragma once

// 1. Glad MUST be before GLFW
#include <glad/glad.h>

// 2. GLFW
#include <GLFW/glfw3.h>

// 3. ImGui MUST be before ImGuizmo
#include "imgui.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ImGuizmo.h"

// 4. GLM
#include <glm/glm.hpp>

#include "AssetManager.h"
#include "CloudFX.h"
#include "ECS/Systems/CameraSystem.h"
#include "ECS/Systems/EditorCamera.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "ECS/Systems/RenderSystem.h"
#include "EditorUI.h"
#include "EventBus.h"
#include "FireFX.h"
#include "HDRSky.h"
#include "NetworkSubsystem.h"
#include "PostProcessor.h"
#include "ProjectConfig.h"
#include "ProjectileSystem.h"
#include "RenderGraph.h"
#include "Renderer.h"
#include "Scene.h"
#include "Scripting/ScriptSystem.h"
#include "SubsystemManager.h"
#include "SunFX.h"

#include <memory>
#include <string>
#include <vector>

class Shader;
class EditorSubsystem;
class RenderLoopSubsystem;
class CoreAppLayer;

struct AppConfig {
  // Sun
  glm::vec3 sunPos;
  glm::vec3 sunDir;
  glm::vec3 sunColor;
  float sunSize;
  float ambientStrength;

  // Fire
  bool fireEnabled;
  glm::vec3 fireOffset;
  float fireSize;
  float fireIntensity;

  // Player / Camera
  float x, y, z;
  float yaw, pitch;

  // Terrain
  int terrainSize;
  float terrainSpacing;

  // Tree (Example entity)
  glm::vec3 treePos;
  glm::vec3 treeScale;

  // Turret
  float turretYaw;
};
struct EntitySaveData {
  char name[64]; // Fixed size string for binary safety
  glm::vec3 pos;
  glm::vec3 rot;
  glm::vec3 scale;
};

// ---------------------------------------------------------------------------
// Focused sub-structs — each groups a coherent set of concerns
// ---------------------------------------------------------------------------

struct RenderSettings {
  float mixVal = 0.5f;
  float shadowStrength = 1.5f;
  float shadowFarPlane = 250.0f;
  float exposure = 1.0f;
  float gamma = 2.2f;

  bool wireframe = false;
  bool disableShadows = false;
  bool disableClouds = false;
  bool disableHDR = true; // Off by default
  bool freezeTime = false;
  float frozenTime = 0.0f;
  bool frustumCulling = true;
};

struct InputSettings {
  float walkStep = 0.03f;
  float runMult = 2.0f;
  float jumpStrength = 0.18f;
  float gravity = 0.01f;
  bool freezePhysics = false;
  float mouseSensitivity = 0.10f;
  float fov = 50.0f;
};

struct SelectionState {
  uint32_t selectedEntityId = 0;
  std::vector<uint32_t> selectedEntities;
  uint32_t lastClickedEntity = 0;

  bool editObjPart = false;
  std::string selectedObjPartName;

  bool editColliderBounds =
      false; // Toggle to intercept gizmo scaling for colliders

  ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
  ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;

  bool renaming = false;
  char renameBuf[128] = "";
  char outlinerFilter[128] = "";
  float focusDistance = 12.0f;
};

struct SkySettings {
  bool solidSky = true;
  float skyHorizon[3] = {0.70f, 0.80f, 0.95f};
  float skyTop[3] = {0.12f, 0.20f, 0.45f};
};

struct PendingActions {
  std::vector<std::string> pendingDropPaths;
  std::vector<std::string> pendingSpawnPaths;
  std::vector<uint32_t> pendingDeleteEntityIds;
  std::vector<std::string> pendingEmptyEntityNames;

  bool requestSaveConfig = false;
  bool requestLoadConfig = false;
  bool requestSaveProjectConfig = false;
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

// ---------------------------------------------------------------------------
// AppState — organized into focused sub-structs
// ---------------------------------------------------------------------------
struct AppState {
  // Window / timing
  GLFWwindow *window = nullptr;
  int scrW = 800;
  int scrH = 600;
  float lastT = 0.0f;

  // Core systems
  Renderer renderer;
  Scene scene;
  SunFX sun;
  CloudFX cloud;
  HDRSky sky;
  FireFX fire;
  PostProcessor postProcessor;
  EditorUI editor;
  ProjectileSystem projectiles;

  // ECS Systems
  RenderSystem renderSystem;
  CameraSystem cameraSystem;
  EditorCamera editorCamera;
  ScriptSystem scriptSystem;
  PhysicsSystem physicsSystem;
  NetworkSubsystem networkSystem;
  RenderGraph renderGraph;
  std::vector<std::string> lastRenderPassOrder;

  // Player ID
  uint32_t playerId = 0;

  // Terrain
  int terrainSize = 10;
  float terrainSpacing = 1.0f;

  // Fire
  bool hasFire = false;

  // Outline shader
  std::unique_ptr<Shader> outlineShader;

  // UI mode
  bool uiMode = true;
  bool escWasDown = false;

  // Play state (controls Lua script execution)
  enum class PlayState { Stopped, Playing, Paused };
  PlayState playState = PlayState::Stopped;

  // --- Focused sub-structs ---
  RenderSettings render;
  InputSettings input;
  SelectionState selection;
  SkySettings skyUI;
  PendingActions pending;
  HistoryState history;

  // Infrastructure
  ProjectConfig projectConfig;
  EventBus events;
  EditorSubsystem *editorSubsystem = nullptr;
  RenderLoopSubsystem *renderLoopSubsystem = nullptr;
  CoreAppLayer *coreAppLayer = nullptr;
  SubsystemManager subsystems;
  AssetManager assets;

  // Hot reload
  std::vector<std::string> hotReloadMessages;
  bool hotReloadEnabled = true;
  bool autoProcessImportQueue = false;
};

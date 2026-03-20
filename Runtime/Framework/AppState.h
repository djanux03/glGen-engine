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
#include "Core/FrameProfiler.h"
#include "NetworkSubsystem.h"
#include "PostProcessor.h"
#include "ProjectConfig.h"
#include "ProjectileSystem.h"
#include "FireflySystem.h"
#include "RenderGraph.h"
#include "Renderer.h"
#include "Scene.h"
#include "Scripting/ScriptSystem.h"
#include "SubsystemManager.h"
#include "SunFX.h"
#include "Terrain/TerrainMaterialSettings.h"
#include "Terrain/TerrainSystem.h"

#include <memory>
#include <string>
#include <vector>

class Shader;
class EditorSubsystem;
class RenderLoopSubsystem;
class CoreAppLayer;
class AudioSubsystem;

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
  int shadowUpdateInterval = 1; // frames between shadow map updates
  float shadowUpdateDistance = 0.5f; // meters
  float shadowUpdateAngle = 2.0f; // degrees
  float exposure = 1.0f;
  float gamma = 2.2f;
  float fogDensity = 0.0025f;
  float fogHeightFalloff = 0.05f;
  glm::vec3 fogColor = glm::vec3(0.55f, 0.65f, 0.78f);
  bool toonEnabled = false;
  int toonSteps = 4;
  float toonMin = 0.12f;
  bool shadowBandEnabled = false;
  int shadowBandSteps = 3;
  float shadowBandSoftness = 0.2f;
  bool ambientRampEnabled = false;
  float ambientRampStrength = 0.6f;
  glm::vec3 ambientRampTop = glm::vec3(0.70f, 0.82f, 0.95f);
  glm::vec3 ambientRampBottom = glm::vec3(0.20f, 0.25f, 0.28f);
  bool rimEnabled = false;
  float rimPower = 2.0f;
  float rimStrength = 0.6f;
  glm::vec3 rimColor = glm::vec3(0.9f, 0.95f, 1.0f);

  bool wireframe = false;
  bool disableShadows = false;
  bool disableClouds = true; // Use lightweight sky-clouds by default
  bool disableHDR = true; // Off by default
  bool freezeTime = false;
  float frozenTime = 0.0f;
  bool frustumCulling = true;
  bool shadowCameraCulling = true;

  glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
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
  // Manual sky colors (used when day/night disabled)
  float skyHorizon[3] = {0.55f, 0.72f, 0.95f};
  float skyTop[3] = {0.22f, 0.42f, 0.82f};
  // Day/Night cycle
  bool dayNightEnabled = false;
  float timeOfDay = 0.35f;  // 0..1 (0=midnight, 0.25=sunrise, 0.5=noon)
  float cycleSpeed = 0.02f; // cycles per minute (set 0 for manual)
  float dayHorizon[3] = {0.55f, 0.72f, 0.95f};
  float dayTop[3] = {0.22f, 0.42f, 0.82f};
  float nightHorizon[3] = {0.02f, 0.03f, 0.08f};
  float nightTop[3] = {0.01f, 0.01f, 0.04f};
  glm::vec3 sunDayColor = glm::vec3(1.0f, 0.95f, 0.85f);
  glm::vec3 sunDuskColor = glm::vec3(1.0f, 0.55f, 0.25f);
  glm::vec3 sunNightColor = glm::vec3(0.1f, 0.15f, 0.3f);
  bool minimalSky = false;
  float skyBackdropBlend = 0.85f;
  float skyFeatureVisibility = 0.08f;
  // Fireflies (night-only)
  bool firefliesEnabled = true;
  int fireflyCount = 120;
  float fireflyRadius = 28.0f;
  float fireflyHeightMin = 0.6f;
  float fireflyHeightMax = 4.0f;
  float fireflySize = 6.0f;
  float fireflyIntensity = 1.2f;
  glm::vec3 fireflyColor = glm::vec3(0.90f, 1.00f, 0.65f);
};

struct TerrainBrushSettings {
  bool enabled = false;
  int mode = 0;   // 0=Raise,1=Lower,2=Add,3=Remove
  int target = 0; // 0=Tree,1=Rock,2=Grass
  float radius = 6.0f;
  float strength = 2.0f; // units per second
  int scatterCount = 6;
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
  FireflySystem fireflies;

  // ECS Systems
  RenderSystem renderSystem;
  CameraSystem cameraSystem;
  EditorCamera editorCamera;
  ScriptSystem scriptSystem;
  PhysicsSystem physicsSystem;
  NetworkSubsystem networkSystem;
  RenderGraph renderGraph;
  std::vector<std::string> lastRenderPassOrder;
  std::unique_ptr<Shader> terrainFlatShader;

  // Player ID
  uint32_t playerId = 0;
  int woodCount = 0;
  uint32_t axeEntity = 0;
  float lastPlayerYaw = 0.0f;
  float lastPlayerPitch = 0.0f;
  bool hasLastPlayerRot = false;

  // Terrain
  int terrainSize = 10;
  float terrainSpacing = 1.0f;
  TerrainSettings terrainSettings;
  TerrainSystem terrainSystem;
  TerrainMaterialSettings terrainMaterial;
  TerrainBrushSettings terrainBrush;

  // Fire
  bool hasFire = false;

  // Outline shader
  std::unique_ptr<Shader> outlineShader;

  // UI mode
  bool uiMode = true;
  bool escWasDown = false;

  // Hotbar System
  enum class HotbarSlot { Axe = 1, Torch = 2 };
  HotbarSlot activeSlot = HotbarSlot::Axe;

  // Viewmodel (axe)
  bool axeEnabled = true;
  glm::vec3 axeOffset = glm::vec3(0.06f, -0.15f, 0.24f);
  glm::vec3 axeRotation = glm::vec3(117.5f, 84.5f, 2.0f); // degrees
  
  // Viewmodel (torch)
  bool torchEnabled = true;
  uint32_t torchEntity = 0;
  glm::vec3 torchOffset = glm::vec3(0.12f, -0.3f, 0.40f); // further right, lower down
  glm::vec3 torchRotation = glm::vec3(-20.0f, -20.0f, 0.0f); // tilted slightly forward
  glm::vec3 torchScale = glm::vec3(0.05f, 0.5f, 0.05f); // long, thin wooden stick
  glm::vec3 axeScale = glm::vec3(0.66f);
  std::string axePath = "assets/playerassets/axe.obj";
  bool usePlayerCameraInEdit = true;
  // Debug: last mouse deltas + camera rotation
  float debugMouseDX = 0.0f;
  float debugMouseDY = 0.0f;
  float debugYaw = 0.0f;
  float debugPitch = 0.0f;
  glm::vec3 debugCamFront = glm::vec3(0.0f, 0.0f, -1.0f);
  glm::vec3 debugCamUp = glm::vec3(0.0f, 1.0f, 0.0f);

  // Magic wand grab (play mode)
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
  // Debug: last grab raycast
  uint32_t debugGrabHitId = 0;
  float debugGrabHitDist = 0.0f;
  std::string debugGrabHitName;
  std::string debugGrabPrefab;
  int debugGrabInstance = -1;
  bool debugGrabMoved = false;

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
  AudioSettings audio;

  // Infrastructure
  ProjectConfig projectConfig;
  EventBus events;
  EditorSubsystem *editorSubsystem = nullptr;
  RenderLoopSubsystem *renderLoopSubsystem = nullptr;
  CoreAppLayer *coreAppLayer = nullptr;
  AudioSubsystem *audioSubsystem = nullptr;
  SubsystemManager subsystems;
  AssetManager assets;
  FrameProfiler profiler;
  float gpuFrameMs = 0.0f;
  float gpuShadowMs = 0.0f;
  float gpuMainMs = 0.0f;
  int glProgramBinds = 0;
  int glTextureBinds = 0;
  int glVaoBinds = 0;
  int glStateChanges = 0;

  // Hot reload
  std::vector<std::string> hotReloadMessages;
  bool hotReloadEnabled = true;
  bool autoProcessImportQueue = false;
  bool iconFontLoaded = false;
  bool audioBackendAvailable = false;
  std::string audioStatus;
};

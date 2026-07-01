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
#include "BlackHoleRenderer.h"
#include "CloudFX.h"
#include "Core/PerformanceSnapshot.h"
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
#include "PlayerControllerSystem.h"
#include "PlayerInteractionSystem.h"
#include "SpaceshipControlSystem.h"
#include "FireflySystem.h"
#include "RenderGraph.h"
#include "Renderer.h"
#include "Scene.h"
#include "Scripting/ScriptSystem.h"
#include "SubsystemManager.h"
#include "SunFX.h"
#include "Terrain/TerrainMaterialSettings.h"
#include "Terrain/TerrainSystem.h"
#include "GameplayState.h"

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
  bool enableCascadedShadows = true;
  int shadowCascadeCount = 4;
  int shadowMapResolution = 2048;
  float shadowCascadeDistance = 700.0f;
  float shadowCascadeLambda = 0.65f;
  float shadowNormalBias = 0.035f;
  float shadowDepthBias = 0.0015f;
  float shadowSoftness = 1.0f;
  bool showShadowCascades = false;
  int shadowUpdateInterval = 1; // frames between shadow map updates
  float shadowUpdateDistance = 0.5f; // meters
  float shadowUpdateAngle = 2.0f; // degrees
  bool shadowStaggeredUpdates = true;
  int shadowCascadeCadence = 2;
  float shadowCascadeDistanceScale = 2.0f;
  float shadowCascadeAngleScale = 1.6f;
  float exposure = 1.0f;
  float gamma = 2.2f;
  float fogDensity = 0.0012f;
  float fogHeightFalloff = 0.018f;
  glm::vec3 fogColor = glm::vec3(0.55f, 0.65f, 0.78f);
  bool aerialPerspectiveEnabled = true;
  float aerialPerspectiveDensity = 0.0014f;
  float aerialPerspectiveStart = 45.0f;
  float aerialPerspectiveHeightFalloff = 0.006f;
  float aerialPerspectiveSkyBlend = 0.72f;
  float aerialPerspectiveSunGlow = 0.38f;
  float aerialPerspectiveDesaturation = 0.35f;
  bool ambientHemisphereEnabled = true;
  float ambientHemisphereIntensity = 1.0f;
  float ambientSkyInfluence = 0.65f;
  float ambientHorizonStrength = 0.42f;
  float ambientTerrainBoost = 1.12f;
  glm::vec3 ambientSkyColor = glm::vec3(0.62f, 0.74f, 0.88f);
  glm::vec3 ambientHorizonColor = glm::vec3(0.50f, 0.58f, 0.48f);
  glm::vec3 ambientGroundColor = glm::vec3(0.18f, 0.21f, 0.16f);
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
  bool disableClouds = false;
  bool disableHDR = true;
  bool freezeTime = false;
  float frozenTime = 0.0f;
  bool frustumCulling = true;
  bool shadowCameraCulling = true;

  glm::mat4 lightSpaceMatrix = glm::mat4(1.0f);
  glm::mat4 lightSpaceMatrices[4] = {glm::mat4(1.0f), glm::mat4(1.0f),
                                     glm::mat4(1.0f), glm::mat4(1.0f)};
  float shadowCascadeSplits[4] = {30.0f, 100.0f, 280.0f, 700.0f};
  int activeShadowCascadeCount = 1;
  bool shadowUsingCsm = false;
  int activeShadowMapResolution = 2048;
};

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



struct SkySettings {
  bool solidSky = true;
  std::string skyHDRPath;
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
  glm::vec3 visualSunColor = glm::vec3(1.0f, 0.86f, 0.58f);
  glm::vec3 visualSunDayColor = glm::vec3(1.0f, 0.88f, 0.62f);
  glm::vec3 visualSunDuskColor = glm::vec3(1.0f, 0.48f, 0.18f);
  glm::vec3 visualSunNightColor = glm::vec3(0.16f, 0.22f, 0.45f);
  bool useBlackHole = false;
  bool blackHoleWorldMode = false;
  float blackHoleAzimuth = 225.0f;
  float blackHoleElevation = 28.0f;
  glm::vec3 blackHoleWorldPosition = glm::vec3(0.0f, 35.0f, -120.0f);
  float blackHoleWorldRadius = 12.0f;
  float blackHoleViewPitchDeg = 0.0f;
  float blackHoleSizeDeg = 5.0f;
  float blackHoleDiskTiltDeg = -28.0f;
  float blackHoleDiskInclinationDeg = 76.0f;
  glm::vec3 blackHoleColor = glm::vec3(1.0f, 0.58f, 0.18f);
  float blackHoleRingIntensity = 4.6f;
  float blackHoleRingWidth = 0.18f;
  float blackHoleDistortion = 0.62f;
  float blackHoleHaloIntensity = 0.24f;
  float blackHoleDiskSpinSpeed = 0.45f;
  float blackHoleDiskFlowShear = 0.22f;
  float blackHoleDiskTurbulence = 0.55f;
  float blackHoleChromaticAberration = 0.08f;
  float blackHoleEclipseStrength = 0.40f;
  float blackHolePhotonRingIntensity = 2.10f;
  float blackHoleDopplerBoost = 0.62f;
  float blackHoleJetIntensity = 0.0f;
  float blackHoleCoronaIntensity = 0.42f;
  float blackHoleStarLensIntensity = 0.70f;
  float blackHoleShadowStrength = 0.82f;
  float blackHoleInnerDiskRadius = 1.25f;
  float blackHoleOuterDiskRadius = 6.8f;
  float blackHoleDiskTemperature = 1.12f;
  float blackHoleDiskDensity = 1.15f;
  float blackHoleLensingStrength = 0.90f;
  float blackHoleBackgroundStarIntensity = 1.15f;
  float blackHoleExposure = 1.0f;
  int blackHoleQuality = 1;
  float skyAtmosphereStrength = 0.28f;
  float skyGradientPower = 1.15f;
  float skyHorizonGlow = 0.16f;
  float skySunDiscIntensity = 14.0f;
  float skySunHaloIntensity = 0.45f;
  float skySunRaysIntensity = 0.12f;
  float skySunDiscSoftness = 0.0014f;
  float skySunHaloSize = 0.18f;
  float skySunRaySharpness = 12.0f;
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
  BlackHoleRenderer blackHole;
  FireFX fire;
  PostProcessor postProcessor;
  EditorUI editor;
  ProjectileSystem projectiles;
  PlayerControllerSystem playerController;
  PlayerInteractionSystem playerInteraction;
  SpaceshipControlSystem spaceshipControl;
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

  // Gameplay (player, viewmodel, grab, debug) — owned struct
  GameplayState gameplay;

  // Terrain
  int terrainSize = 10;
  float terrainSpacing = 1.0f;
  TerrainSettings terrainSettings;
  TerrainSystem terrainSystem;
  TerrainMaterialSettings terrainMaterial;

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
  SkySettings skyUI;
  PlayPerfHudSettings playPerfHud;
  PerformanceLogSettings performanceLog;
  FramePerformanceSnapshot performance;

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
  float gpuMainSkyMs = 0.0f;
  float gpuMainTerrainMs = 0.0f;
  float gpuMainSceneMs = 0.0f;
  float gpuMainPostMs = 0.0f;
  int shadowCascadesUpdated = 0;
  bool shadowCascadeStaggered = false;
  int glProgramBinds = 0;
  int glTextureBinds = 0;
  int glVaoBinds = 0;
  int glStateChanges = 0;
  uint64_t performanceLogFrameIndex = 0;
  float performanceLogSummaryTimer = 0.0f;
  float performanceLogSpikeCooldown = 0.0f;

  // Hot reload
  std::vector<std::string> hotReloadMessages;
  bool hotReloadEnabled = true;
  bool autoProcessImportQueue = false;
  bool iconFontLoaded = false;
};

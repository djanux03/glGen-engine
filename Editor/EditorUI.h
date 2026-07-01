#pragma once
#include "EditorToolbar.h"
#include "Core/FrameProfiler.h"
#include "Core/PerformanceSnapshot.h"
#include "Terrain/TerrainMaterialSettings.h"
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// Forward declarations
class SunFX;
class FireFX;
class CloudFX;
class HDRSky;
class Scene;
class OBJModel;
class ProjectileSystem;
class PostProcessor;
class Registry;
class EventBus;
struct ProjectConfig;
class AssetManager;
struct VisibilityStats;
struct TerrainSettings;
class TerrainSystem;
class EditorCamera;
struct AudioSettings;
struct TerrainBrushSettings;
struct SelectionState;
struct HistoryState;
struct PendingActions;
struct GameplayState;

// ---------------------------------------------------------------------------
// Selection state for outliner / gizmo
// ---------------------------------------------------------------------------
// Selection state is now passed via SelectionState directly

// ---------------------------------------------------------------------------
// EditorContext — single struct replaces the 14-parameter draw() signature.
// Add new systems here instead of growing the function signature.
// ---------------------------------------------------------------------------
struct EditorContext {
  // UI mode
  bool &uiMode;

  // Player tuning
  float &walkStep;
  float &runMult;
  float &jumpStrength;
  float &gravity;
  bool &freezePhysics;
  bool &creativeFlight;
  float &mouseSensitivity;
  float &fov;

  // Systems
  SunFX &sun;
  FireFX &fire;
  CloudFX &cloud;
  HDRSky &sky;
  ProjectileSystem &projectiles;
  PostProcessor &postProcessor;
  Scene &scene;
  EventBus &events;
  ProjectConfig &projectConfig;
  AssetManager &assets;

  // Terrain
  int &terrainSize;
  float &terrainSpacing;
  TerrainSettings &terrainSettings;
  TerrainMaterialSettings &terrainMaterial;
  TerrainSystem &terrainSystem;
  EditorCamera &editorCamera;
  TerrainBrushSettings &terrainBrush;

  // Sky params stored in AppState
  bool &solidSky;
  std::string &skyHDRPath;
  float *skyHorizon; // float[3]
  float *skyTop;     // float[3]
  bool &dayNightEnabled;
  float &timeOfDay;
  float &cycleSpeed;
  float *dayHorizon; // float[3]
  float *dayTop;     // float[3]
  float *nightHorizon; // float[3]
  float *nightTop;     // float[3]
  glm::vec3 &sunDayColor;
  glm::vec3 &sunDuskColor;
  glm::vec3 &sunNightColor;
  glm::vec3 &visualSunColor;
  glm::vec3 &visualSunDayColor;
  glm::vec3 &visualSunDuskColor;
  glm::vec3 &visualSunNightColor;
  bool &useBlackHole;
  bool &blackHoleWorldMode;
  float &blackHoleAzimuth;
  float &blackHoleElevation;
  glm::vec3 &blackHoleWorldPosition;
  float &blackHoleWorldRadius;
  float &blackHoleViewPitchDeg;
  float &blackHoleSizeDeg;
  float &blackHoleDiskTiltDeg;
  float &blackHoleDiskInclinationDeg;
  glm::vec3 &blackHoleColor;
  float &blackHoleRingIntensity;
  float &blackHoleRingWidth;
  float &blackHoleDistortion;
  float &blackHoleHaloIntensity;
  float &blackHoleDiskSpinSpeed;
  float &blackHoleDiskFlowShear;
  float &blackHoleDiskTurbulence;
  float &blackHoleChromaticAberration;
  float &blackHoleEclipseStrength;
  float &blackHolePhotonRingIntensity;
  float &blackHoleDopplerBoost;
  float &blackHoleJetIntensity;
  float &blackHoleCoronaIntensity;
  float &blackHoleStarLensIntensity;
  float &blackHoleShadowStrength;
  float &blackHoleInnerDiskRadius;
  float &blackHoleOuterDiskRadius;
  float &blackHoleDiskTemperature;
  float &blackHoleDiskDensity;
  float &blackHoleLensingStrength;
  float &blackHoleBackgroundStarIntensity;
  float &blackHoleExposure;
  int &blackHoleQuality;
  float &skyAtmosphereStrength;
  float &skyGradientPower;
  float &skyHorizonGlow;
  float &skySunDiscIntensity;
  float &skySunHaloIntensity;
  float &skySunRaysIntensity;
  float &skySunDiscSoftness;
  float &skySunHaloSize;
  float &skySunRaySharpness;
  bool &minimalSky;
  float &skyBackdropBlend;
  float &skyFeatureVisibility;
  bool &firefliesEnabled;
  int &fireflyCount;
  float &fireflyRadius;
  float &fireflyHeightMin;
  float &fireflyHeightMax;
  float &fireflySize;
  float &fireflyIntensity;
  glm::vec3 &fireflyColor;

  // Renderer controls
  float &shadowStrength;
  float &shadowFarPlane;
  bool &enableCascadedShadows;
  int &shadowCascadeCount;
  int &shadowMapResolution;
  float &shadowCascadeDistance;
  float &shadowCascadeLambda;
  float &shadowNormalBias;
  float &shadowDepthBias;
  float &shadowSoftness;
  bool &showShadowCascades;
  int &shadowUpdateInterval;
  float &shadowUpdateDistance;
  float &shadowUpdateAngle;
  bool &shadowStaggeredUpdates;
  int &shadowCascadeCadence;
  float &shadowCascadeDistanceScale;
  float &shadowCascadeAngleScale;
  bool &shadowCameraCulling;
  float &exposure;
  float &gamma;
  float &fogDensity;
  float &fogHeightFalloff;
  glm::vec3 &fogColor;
  bool &aerialPerspectiveEnabled;
  float &aerialPerspectiveDensity;
  float &aerialPerspectiveStart;
  float &aerialPerspectiveHeightFalloff;
  float &aerialPerspectiveSkyBlend;
  float &aerialPerspectiveSunGlow;
  float &aerialPerspectiveDesaturation;
  bool &ambientHemisphereEnabled;
  float &ambientHemisphereIntensity;
  float &ambientSkyInfluence;
  float &ambientHorizonStrength;
  float &ambientTerrainBoost;
  glm::vec3 &ambientSkyColor;
  glm::vec3 &ambientHorizonColor;
  glm::vec3 &ambientGroundColor;
  bool &toonEnabled;
  int &toonSteps;
  float &toonMin;
  bool &shadowBandEnabled;
  int &shadowBandSteps;
  float &shadowBandSoftness;
  bool &ambientRampEnabled;
  float &ambientRampStrength;
  glm::vec3 &ambientRampTop;
  glm::vec3 &ambientRampBottom;
  bool &rimEnabled;
  float &rimPower;
  float &rimStrength;
  glm::vec3 &rimColor;

  // Render debug toggles
  bool &wireframe;
  bool &disableShadows;
  bool &disableClouds;
  bool &disableHDR;
  bool &freezeTime;
  GameplayState &gameplay;

  // Audio (owned by AudioSubsystem)
  AudioSettings &audio;
  bool &audioBackendAvailable;
  std::string &audioStatus;

  // Frame stats
  float dt;
  int entityCount;
  int particleCount;
  int visibleDrawn = 0;
  int visibleCulled = 0;
  int drawCallsMain = 0;
  int drawCallsShadow = 0;
  int instancedDrawCallsMain = 0;
  int instancedDrawCallsShadow = 0;
  bool &cullingEnabled;
  const std::vector<std::string> *renderPassOrder = nullptr;
  bool &hotReloadEnabled;
  bool &autoProcessImportQueue;
  bool &iconFontLoaded;
  const std::vector<std::string> *hotReloadMessages = nullptr;
  HistoryState &history;
  PendingActions &pending;
  const std::vector<FrameProfiler::Sample> *cpuSamples = nullptr;
  float gpuFrameMs = 0.0f;
  float gpuShadowMs = 0.0f;
  float gpuMainMs = 0.0f;
  int glProgramBinds = 0;
  int glTextureBinds = 0;
  int glVaoBinds = 0;
  int glStateChanges = 0;
  const FramePerformanceSnapshot &performance;

  // Selection State
  SelectionState &selection;

  // Play state (reference into AppState)
  int &playState; // 0=Stopped, 1=Playing, 2=Paused
};

// ---------------------------------------------------------------------------
// Output from EditorUI::draw()
// ---------------------------------------------------------------------------
struct EditorUIOutput {
  bool wantCaptureMouse = false;
  bool wantCaptureKeyboard = false;
  bool terrainDirty = false;
  bool sceneModified = false;
  std::vector<std::string> consoleCommands;

  bool saveRequested = false;
  bool loadRequested = false;

  // File browser result — if non-empty, spawn entity from this path
  std::string spawnPath;
  // Delete request — if non-zero, delete this entity
  uint32_t deleteEntityId = 0;
};

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// EditorUI
// ---------------------------------------------------------------------------
class EditorUI {
public:
  // Main parameter panel (all systems)
  EditorUIOutput draw(EditorContext &ctx);

  // Gizmo & Outliner (separate window)
  bool drawGizmo(bool uiMode, const glm::mat4 &view,
                 const glm::mat4 &projection, Scene &scene, SunFX &sun,
                 EventBus &events, SelectionState &sel,
                 glm::vec3 &cameraPos);

  // Toolbar state — accessible from outside for gizmo/wireframe sync
  ToolbarState toolbarState;

  // Component inspector for selected entity
  bool drawInspector(EditorContext &ctx);
  void drawScriptEditor(EditorContext &ctx);
  void toggleConsole() { mShowLog = !mShowLog; }
  void setConsoleOpen(bool open) { mShowLog = open; }
  bool isConsoleOpen() const { return mShowLog; }

private:
  // File browser state
  bool mShowFileBrowser = false;
  std::string mBrowsePath;
  char mPathInput[512] = "";
  bool mResetLayout = true;
  bool mLockLayout = true; // Panels locked by default
  char mAssetSearch[128] = "";

  // Window visibility flags
  bool mShowHierarchy = true;
  bool mShowInspector = true;
  bool mShowAssets = true;
  bool mShowEnvironment = true;
  bool mShowLog = true;
  bool mShowStats = true;
  bool mShowProfiler = true;
  bool mShowScriptEditor = false;

  // Script editor state
  std::string mScriptEditorPath;
  char mScriptEditorBuf[8192] = {};
  bool mScriptEditorDirty = false;

  // Console state
  bool mConsoleAutoScroll = true;
  bool mFilterInfo = true;
  bool mFilterWarn = true;
  bool mFilterError = true;
  char mConsoleSearch[128] = "";
  char mConsoleInput[256] = "";
  std::vector<std::string> mConsoleHistory;
  int mConsoleHistoryPos = -1;
  std::vector<std::string> mPendingConsoleCommands;

  // FPS history for graph
  static constexpr int kFpsHistorySize = 120;
  float mFpsHistory[kFpsHistorySize] = {};
  int mFpsHistoryIdx = 0;

  // Internal draw helpers
  void drawMainMenuBar(EditorContext &ctx);
  void drawHierarchy(EditorContext &ctx);
  void drawAssets(EditorContext &ctx);
  void drawAssetsContent(EditorContext &ctx);
  void drawEnvironment(EditorContext &ctx);
  void drawLog(EditorContext &ctx);
  void drawStats(EditorContext &ctx);
  void drawProfiler(EditorContext &ctx);
  static int consoleInputCallback(ImGuiInputTextCallbackData *data);
};

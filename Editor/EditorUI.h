#pragma once
#include <glm/glm.hpp>
#include <cstdint>
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
class Registry;
class EventBus;
struct ProjectConfig;
class AssetManager;
struct VisibilityStats;

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
  float &mouseSensitivity;
  float &fov;

  // Systems
  SunFX &sun;
  FireFX &fire;
  CloudFX &cloud;
  HDRSky &sky;
  ProjectileSystem &projectiles;
  Scene &scene;
  EventBus &events;
  ProjectConfig &projectConfig;
  AssetManager &assets;

  // Terrain
  int &terrainSize;
  float &terrainSpacing;

  // Sky params stored in AppState
  bool &solidSky;
  float *skyHorizon; // float[3]
  float *skyTop;     // float[3]

  // Renderer controls
  float &shadowStrength;
  float &shadowFarPlane;
  float &exposure;
  float &gamma;

  // Render debug toggles
  bool &wireframe;
  bool &disableShadows;
  bool &disableClouds;
  bool &freezeTime;

  // Frame stats
  float dt;
  int entityCount;
  int particleCount;
  int visibleDrawn = 0;
  int visibleCulled = 0;
  bool &cullingEnabled;
  const std::vector<std::string> *renderPassOrder = nullptr;
  bool &hotReloadEnabled;
  bool &autoProcessImportQueue;
  const std::vector<std::string> *hotReloadMessages = nullptr;
  const std::vector<std::string> *historyLabels = nullptr;
  int historyIndex = -1;
};

// ---------------------------------------------------------------------------
// Output from EditorUI::draw()
// ---------------------------------------------------------------------------
struct EditorUIOutput {
  bool wantCaptureMouse = false;
  bool wantCaptureKeyboard = false;
  bool terrainDirty = false;

  bool saveRequested = false;
  bool loadRequested = false;

  // File browser result — if non-empty, spawn entity from this path
  std::string spawnPath;
  // Delete request — if non-zero, delete this entity
  uint32_t deleteEntityId = 0;
};

// ---------------------------------------------------------------------------
// Selection state for outliner / gizmo
// ---------------------------------------------------------------------------
struct EditorSelectionState {
  uint32_t &selectedEntityId;
  std::vector<uint32_t> &selectedEntities;
  uint32_t &lastClickedEntity;

  bool &editObjPart;
  std::string &selectedObjPartName;

  int &gizmoOp;
  int &gizmoMode;

  bool &renaming;
  char *renameBuf;
  char *outlinerFilter;
  float &focusDistance;
};

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
                 EventBus &events, EditorSelectionState &sel,
                 glm::vec3 &cameraPos);

  // Component inspector for selected entity
  bool drawInspector(bool uiMode, Registry &reg, uint32_t selectedEntityId);

private:
  // File browser state
  bool mShowFileBrowser = false;
  std::string mBrowsePath;
  char mPathInput[512] = "";
  char mAssetSearch[128] = "";
  bool mShowPanelEditor = true;
  bool mShowPanelAssetBrowser = true;
  bool mShowPanelHistory = true;
  bool mShowPanelLogConsole = true;
};

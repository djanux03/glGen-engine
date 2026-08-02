#pragma once

// The old engine's editor UI on the Vulkan runtime: same theme
// (EditorTheme), toolbar (EditorToolbar), panel set (Hierarchy / Inspector /
// Assets+Console / Environment / Statistics) and ImGuizmo transform gizmo,
// driven entirely by EngineCore state — no OpenGL anywhere.

#include "EditorState.h"   // SelectionState (Runtime/Framework, GL-free)
#include "EditorToolbar.h" // ToolbarState + toolbar strip (Editor/, GL-free)
#include "TerrainSettings.h" // TerrainSettings (Terrain Generator panel's staged copy)

#include <array>
#include <glm/glm.hpp>
#include <string>

class Scene;
class AssetManager;
class PhysicsSystem;
class ScriptSystem;
class VkTerrainSubsystem;
struct EditorCameraSettings;

namespace vkrhi {
class VulkanRenderer;
}

class VkEditor {
public:
  struct Context {
    Scene &scene;
    AssetManager &assets;
    PhysicsSystem &physics;
    vkrhi::VulkanRenderer &renderer;
    float dt = 0.0f;
    bool *simulatePhysics = nullptr;
    std::string assetDir;
    // Nullable: only set once VkTerrainSubsystem exists (main.cpp), same as
    // the other systems above are always non-null in practice but this one
    // predates VkEditor by less. Non-owning.
    VkTerrainSubsystem *terrainSubsystem = nullptr;
    // The free-fly viewport camera's tuning (main.cpp's EditorCamera),
    // edited from Environment > Camera. Nullable, non-owning.
    EditorCameraSettings *editorCamera = nullptr;
    // Drives the Console tab's Lua prompt. Nullable, non-owning.
    ScriptSystem *scriptSystem = nullptr;
  };

  // Phase 5 brush-tool state (Terrain Brush panel). AddVegetation/
  // RemoveVegetation modes are a separable follow-up -- height editing
  // only for now -- but the enum stays easy to extend without reshuffling.
  struct TerrainBrushSettings {
    bool enabled = false;
    enum class Mode { Raise, Lower } mode = Mode::Raise;
    float radius = 6.0f;
    float strength = 2.0f;
  };

  // Draws the full editor frame (menu bar, toolbar, panels, gizmo). `view` /
  // `proj` are the scene camera matrices; pass proj WITHOUT the Vulkan Y
  // flip — ImGuizmo handles screen mapping itself. Returns true when the
  // scene was modified this frame.
  bool draw(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);

  // True from the moment Play is pressed until Stop is pressed (Pause keeps
  // this true -- pausing freezes the simulation, it doesn't leave play mode).
  // main.cpp uses this to decide whether the player character or the
  // free-fly editor camera drives the rendered view and mouse/keyboard.
  bool isInPlayMode() const { return mInPlayMode; }

  // Programmatic equivalent of clicking "> Play" (snapshots the scene, sets
  // *ctx.simulatePhysics) -- for headless smoke/verification runs that need
  // play mode without a mouse. The toolbar button (drawMenuBar) now just
  // calls this too, so there's exactly one entry/exit path to keep in sync.
  void requestPlay(Context &ctx);

  uint32_t createPlayerEntity(Context &ctx);
  bool hasPlayerEntity(Context &ctx) const;
  void drawColliderOutlines(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);
  void drawWireframeOverlay(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);

  void saveAllSettings(Context &ctx);
  void loadAllSettings(Context &ctx);

  SelectionState selection;
  ToolbarState toolbar;
  TerrainBrushSettings brush;

private:
  void drawMenuBar(Context &ctx);
  void drawHierarchy(Context &ctx);
  bool drawInspector(Context &ctx);
  void drawAssetsContent(Context &ctx);
  void drawLog();
  void drawStats(Context &ctx);
  void drawEnvironment(Context &ctx);
  // Sub-section header: SeparatorText in the normal tabbed view, lazy
  // filter-group header while the Environment filter is active.
  void envSection(const char *name);
  // Environment tab contents (each also flattens into the filtered view --
  // see mEnvFilter).
  void drawEnvSky(Context &ctx);
  void drawEnvLight(Context &ctx);
  void drawEnvFog(Context &ctx);
  void drawEnvCamera(Context &ctx);
  void drawEnvPost(Context &ctx);
  void drawEnvStyle(Context &ctx);
  void drawEnvTerrainTab(Context &ctx);
  void drawEnvDebug(Context &ctx);
  void drawTerrainBrush(Context &ctx);
  void drawTerrainGenerator(Context &ctx);
  void drawTerrainMaterials(Context &ctx);
  void drawBiomeLighting(Context &ctx);
  // Fullscreen accent border + "PLAY -- Esc to stop" pill while in play
  // mode: the unmissable answer to "which mode am I in?".
  void drawPlayModeOverlay(Context &ctx);
  bool drawGizmo(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);
  // Reconstructs a world-space ray from the current mouse position + view/
  // proj, and (if brush.enabled, left mouse held, and the gizmo isn't being
  // manipulated) raycasts terrain and applies the brush on a hit.
  void updateTerrainBrush(Context &ctx, const glm::mat4 &view,
                          const glm::mat4 &proj);

  // Destroys an entity plus its Jolt body (the engine's render/physics
  // systems pick the removal up on their next update).
  void deleteEntity(Context &ctx, uint32_t id);
  void releasePhysicsBodies(Context &ctx);

  // Shared by the "[] Stop" button and the Escape-key shortcut (Escape is
  // the only way out of play mode once the mouse is captured for character
  // look, since the button itself isn't clickable with a hidden cursor).
  void stopPlayMode(Context &ctx);

  void drawMultiViewStudio(Context &ctx);
  void drawQuadrantViewModesContent(Context &ctx);
  void drawQuadrantWireframeContent(Context &ctx);
  void drawQuadrantPhysicsContent(Context &ctx);
  void drawQuadrantProfilerContent(Context &ctx);

  // Window visibility (same defaults as the old editor)
  bool mShowHierarchy = true;
  bool mShowInspector = true;
  bool mShowAssets = true;
  bool mShowEnvironment = true;
  bool mShowLog = true;
  bool mShowStats = true;
  bool mShowMultiViewStudio = true;

  enum class ViewMode { Shaded, Wireframe, Normals, DepthSSAO, PBRHeatmap } mViewMode = ViewMode::Shaded;
  enum class SplitLayout { Grid4Way, Split2Horizontal, Split2Vertical, Tabbed } mSplitLayout = SplitLayout::Grid4Way;
  bool mShowEntityAABBs = false;
  bool mShowLightGizmos = true;

  // Windows > Reset Layout: forces every panel back to the default tiled
  // layout for one frame (overrides whatever imgui_glgenvk.ini remembers).
  bool mApplyDefaultLayout = false;

  // Environment settings filter. While non-empty, the tab bar is replaced
  // by a flat list of matching rows across every tab, grouped under lazy
  // section headers. `section()` marks the current group; `row(label)`
  // decides whether a control renders and emits the pending header before
  // the group's first match.
  struct EnvRowFilter {
    char query[64] = "";
    const char *pendingSection = nullptr; // header not yet emitted
    const char *currentSection = nullptr; // matched against, like row labels
    bool active() const { return query[0] != '\0'; }
    void section(const char *name) {
      pendingSection = name;
      currentSection = name;
    }
    bool row(const char *label);
  };
  EnvRowFilter mEnvFilter;

  // Terrain Generator section (Environment > Terrain Generator): staged
  // settings, edited freely and auto-applied (regenerate()) when the user
  // finishes editing a control (mouse released after a drag, not every
  // intermediate frame). Lazily seeded from the live TerrainSettings the
  // first time the section is drawn (see mTerrainGeneratorSeeded).
  TerrainSettings mTerrainGeneratorSettings;
  bool mTerrainGeneratorSeeded = false;

  // Terrain Materials section (R2): staged text-edit buffers for the 5
  // material slots' file paths, since ImGui::InputText needs a fixed char
  // buffer, not std::string. Seeded once from the live Params on first
  // draw (mTerrainMaterialsSeeded), edited freely, only written back to
  // Params (and marked dirty for VulkanRenderer to reload) when the user
  // clicks "Reload Textures" -- typing a path doesn't hot-reload every
  // keystroke.
  struct TerrainMaterialPanelState {
    char albedoPath[256] = "";
    char normalPath[256] = "";
    char roughnessPath[256] = "";
    float tiling = 8.0f;
  };
  std::array<TerrainMaterialPanelState, 5> mTerrainMaterialPanel;
  bool mTerrainMaterialsSeeded = false;

  // Console state
  bool mConsoleAutoScroll = true;
  bool mFilterInfo = true;
  bool mFilterWarn = true;
  bool mFilterError = true;
  char mConsoleSearch[128] = "";
  // Lua prompt at the bottom of the Console tab.
  char mConsoleInput[512] = "";

  // Assets browser state
  char mAssetSearch[128] = "";
  std::string mBrowsePath;

  // FPS history for the stats graph
  static constexpr int kFpsHistorySize = 120;
  float mFpsHistory[kFpsHistorySize] = {};
  int mFpsHistoryIdx = 0;

  std::string mScenePath; // save/load target (defaults under assetDir)

  // Refreshed from Context every draw() so drawLog(), which takes no Context,
  // can reach the Lua VM. Non-owning.
  ScriptSystem *mScriptSystem = nullptr;

  // Play/Pause/Stop: entering Play snapshots the scene so Stop can revert
  // whatever runtime/physics changes happened while simulating, mirroring
  // the old editor's play-mode semantics.
  bool mInPlayMode = false;
  std::string mPlayModeSnapshotPath; // defaults under assetDir
};

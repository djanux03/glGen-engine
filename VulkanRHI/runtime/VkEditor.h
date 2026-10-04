#pragma once

// The editor UI on the Vulkan runtime, driven entirely by EngineCore state --
// no OpenGL anywhere.
//
// Layout is a fixed shell (see ui/UIShell.h) rather than floating windows:
// a toolbar, a Scene rail on the left, a contextual Properties rail on the
// right (entity components when something is selected, world settings when
// not), a collapsible Console/Assets drawer, and a stats overlay -- with the
// 3D viewport owning everything in between. Debug tooling is off by default
// and reachable from View > Debug or the Ctrl+P command palette.
//
// Style and typography live in ui/UITheme.h and ui/UIFonts.h; shared widget
// primitives in ui/UIWidgets.h.

#include "EditorState.h"   // SelectionState (Runtime/Framework, GL-free)
#include "EditorToolbar.h" // ToolbarState (gizmo op/space/snap -- strip is gone)
#include "TerrainSettings.h" // TerrainSettings (Terrain Generator panel's staged copy)
#include "ui/UICommandPalette.h"
#include "ui/UIShell.h"

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

  // The counterpart to requestPlay: reverts the scene to the pre-play
  // snapshot and leaves play mode. Public for the same reason -- the Escape
  // shortcut, the toolbar's Stop button and Lua's game.stop() all need it.
  void stopPlayMode(Context &ctx);

  uint32_t createPlayerEntity(Context &ctx);
  uint32_t createSpaceshipEntity(Context &ctx);
  // Ctrl+D. Returns the new entity, or 0 if nothing was selected.
  uint32_t duplicateSelection(Context &ctx);
  // Entity the viewport camera should frame (F), consumed by main.cpp which
  // owns the EditorCamera. 0 = nothing pending.
  uint32_t takeFocusRequest() {
    const uint32_t id = mFocusRequest;
    mFocusRequest = 0;
    return id;
  }
  // Position the viewport camera should be re-seeded to (session restore or
  // terrain creation placing it on freshly created ground). Returns false if nothing pending.
  // Must go through EditorCamera::seed: it owns the pose and overwrites
  // params().camPos every frame.
  void requestCameraSeed(const glm::vec3 &pos, float yaw = -999.0f, float pitch = -999.0f) {
    mCameraSeedPos = pos;
    mCameraSeedYaw = yaw;
    mCameraSeedPitch = pitch;
    mCameraSeedHasAngles = (yaw != -999.0f || pitch != -999.0f);
    mCameraSeedPending = true;
  }
  bool takeCameraSeedRequest(glm::vec3 &outPos, float &outYaw, float &outPitch) {
    if (!mCameraSeedPending)
      return false;
    outPos = mCameraSeedPos;
    if (mCameraSeedHasAngles) {
      if (mCameraSeedYaw != -999.0f)
        outYaw = mCameraSeedYaw;
      if (mCameraSeedPitch != -999.0f)
        outPitch = mCameraSeedPitch;
    }
    mCameraSeedPending = false;
    mCameraSeedHasAngles = false;
    return true;
  }
  bool takeCameraSeedRequest(glm::vec3 &outPos) {
    float dummyYaw = -999.0f, dummyPitch = -999.0f;
    return takeCameraSeedRequest(outPos, dummyYaw, dummyPitch);
  }
  void onTerrainCreated(Context &ctx);
  void createWoodlandSwamp(Context &ctx);
  bool hasPlayerEntity(Context &ctx) const;
  void drawColliderOutlines(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);
  void drawWireframeOverlay(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);
  // Nearest entity under a screen position, 0 if none. Public so a headless
  // run can exercise the ray/bounds math without a mouse.
  uint32_t pickAt(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj,
                  const ImVec2 &screenPos);
  // Click-to-select in the viewport: casts a ray through the cursor and
  // selects the nearest entity whose world bounds it hits. Clicking empty
  // space clears the selection.
  void updatePicking(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);
  // Accent-colored bracket box around every selected entity, so selection is
  // visible in the scene and not only in the Scene rail.
  void drawSelectionOutline(Context &ctx, const glm::mat4 &view,
                            const glm::mat4 &proj);

  void saveAllSettings(Context &ctx);
  void loadAllSettings(Context &ctx);
  // World Map panel: a top-down view of the whole bounded world with a "you
  // are here" marker. Island layout is a global property that no in-engine
  // camera can show, so without this the only way to know what the world
  // looks like is to sail around it.
  void drawWorldMap(Context &ctx);
  // Loads ONLY graphics_settings.json, and only the first time it is asked.
  //
  // This exists because of an ordering bug that made every headless sun
  // override silently do nothing. loadAllSettings() used to run lazily inside
  // the first draw() -- which happens on frame 1, AFTER main()'s
  // GLGEN_SMOKE_* pose block and AFTER GLGEN_SCRIPT have both set their
  // params. The file then overwrote them. GLGEN_SMOKE_DUSK / _NIGHT /
  // _SUNVIEW / _MOONVIEW set nothing but lightYaw/lightPitch, so all four
  // were dead, and `render.params{sunPitch=...}` from a script was dead with
  // them. Saved settings are DEFAULTS; the pose and the script are overrides,
  // so the file has to be read before either of them runs.
  //
  // VkEditorSubsystem::initialize() calls this; loadAllSettings() then skips
  // the graphics half and restores only the editor session (layout, scene
  // path), which genuinely does belong on the first frame.
  // Takes the renderer rather than its nested Params because this header only
  // forward-declares vkrhi::VulkanRenderer; a reference to an incomplete type
  // is fine, naming a type nested inside it is not.
  void loadGraphicsSettingsOnce(const std::string &assetDir,
                                vkrhi::VulkanRenderer &renderer);
  // Records which scene was open and whether terrain existed, so a restart
  // resumes where you left off. Written by saveAllSettings and by Save Scene.
  void saveSession(Context &ctx);

  // Switches active scene cleanly: stops play mode, clears selection & physics,
  // loads the new scene, optionally adjusts camera to framed view, and saves session.
  bool switchScene(Context &ctx, const std::string &scenePath,
                   bool setCamera = false,
                   glm::vec3 camPos = glm::vec3(0.0f),
                   float pitch = 0.0f,
                   float yaw = 0.0f);

  SelectionState selection;
  ToolbarState toolbar;
  TerrainBrushSettings brush;

private:
  void drawMenuBar(Context &ctx);
  // The single toolbar strip: tools left, play transport centered, search
  // right. Replaces both the old EditorToolbar::draw strip and the play
  // buttons that used to live in the menu bar.
  void drawToolbar(Context &ctx);
  // Left rail.
  void drawScenePanel(Context &ctx);
  // Right rail. Dispatches to entity or world properties by selection.
  bool drawPropertiesPanel(Context &ctx);
  bool drawEntityProperties(Context &ctx);
  void drawWorldProperties(Context &ctx);
  // Bottom drawer (Console / Assets tabs).
  void drawDrawer(Context &ctx);
  void drawAssetsContent(Context &ctx);
  void drawLog();
  // In-viewport overlay, not a panel.
  void drawStatsHud(Context &ctx);
  // Everything demoted out of the default layout: view modes, wireframe,
  // physics lab, profiler. Opened from View > Debug or the palette.
  void drawDebugWindow(Context &ctx);
  // Populates mPalette. Called once, lazily, from draw().
  void registerCommands(Context &ctx);

  // ── World-panel property grid bookkeeping ────────────────────────────
  // envSection() closes the previous section and opens the next as a
  // collapsible card; envRow() emits the label half of a two-column row and
  // returns false when the enclosing section is collapsed (so the caller
  // skips the control entirely); envEnd() closes the last one open.
  void envSection(const char *name);
  bool envRow(const char *label);
  void envEnd();
  // A full-width row: no label column, control spans the panel. For button
  // groups and anything that supplies its own labelling.
  bool envWideRow(const char *label);
  // World-panel content. Each opens its own collapsible sections via
  // envSection(); drawWorldProperties() groups them under captions.
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
  // Gameplay HUD (score / timer / banner) declared by a Lua script through
  // game.hud{}. Replaces the play-mode banner while a game is running.
  void drawGameHud(Context &ctx);
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


  void drawQuadrantViewModesContent(Context &ctx);
  void drawQuadrantWireframeContent(Context &ctx);
  void drawQuadrantPhysicsContent(Context &ctx);
  void drawQuadrantProfilerContent(Context &ctx);

  // Shell layout (rail widths, collapse flags, drawer tab). Persisted by
  // saveAllSettings, not by imgui.ini -- the layout is part of the editor's
  // own settings now, so it is reproducible rather than whatever the user
  // last dragged a floating window to.
  UIShell::State mShell;
  UI::CommandPalette mPalette;
  bool mCommandsRegistered = false;

  // Overlay + demoted windows.
  bool mShowStatsHud = true;
  bool mShowDebugWindow = false;

  enum class ViewMode { Shaded, Wireframe, Normals, DepthSSAO, PBRHeatmap } mViewMode = ViewMode::Shaded;
  bool mShowEntityAABBs = false;
  bool mShowLightGizmos = true;

  // Open/closed state of the current World-panel section, and whether a
  // property row is legal right now. See envSection/envRow/envEnd.
  bool mEnvSectionOpen = false;

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
    char heightPath[256] = "";
    float reliefDepth = .04f;
    float tiling = 8.0f;
  };
  std::array<TerrainMaterialPanelState, 5> mTerrainMaterialPanel;
  bool mTerrainMaterialsSeeded = false;
  int mScenerySelection = -1;

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
  // Reload the last scene (and its terrain) at startup. A fresh checkout has
  // no session file at all, so the engine still boots empty the first time.
  bool mRestoreSessionOnStartup = true;
  // Guards loadGraphicsSettingsOnce() so the lazy first-frame path cannot
  // re-apply the file over a pose or script override.
  bool mGraphicsSettingsLoaded = false;
  // World map texture, generated once per terrain (regeneration invalidates
  // it via mWorldMapSeed).
  void *mWorldMapTexture = nullptr; // VkDescriptorSet from the ImGui backend
  int mWorldMapPixels = 0;
  float mWorldMapExtent = 0.0f;
  uint32_t mWorldMapSeed = 0xFFFFFFFFu;
  bool mWorldMapOpen = false;
  uint32_t mFocusRequest = 0;
  bool mCameraSeedPending = false;
  bool mCameraSeedHasAngles = false;
  glm::vec3 mCameraSeedPos{0.0f};
  float mCameraSeedYaw = 0.0f;
  float mCameraSeedPitch = 0.0f;

  // Refreshed from Context every draw() so drawLog(), which takes no Context,
  // can reach the Lua VM. Non-owning.
  ScriptSystem *mScriptSystem = nullptr;

  // Play/Pause/Stop: entering Play snapshots the scene so Stop can revert
  // whatever runtime/physics changes happened while simulating, mirroring
  // the old editor's play-mode semantics.
  bool mInPlayMode = false;
  std::string mPlayModeSnapshotPath; // defaults under assetDir
};

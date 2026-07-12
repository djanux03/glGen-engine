#pragma once

// The old engine's editor UI on the Vulkan runtime: same theme
// (EditorTheme), toolbar (EditorToolbar), panel set (Hierarchy / Inspector /
// Assets+Console / Environment / Statistics) and ImGuizmo transform gizmo,
// driven entirely by EngineCore state — no OpenGL anywhere.

#include "EditorState.h"   // SelectionState (Runtime/Framework, GL-free)
#include "EditorToolbar.h" // ToolbarState + toolbar strip (Editor/, GL-free)

#include <glm/glm.hpp>
#include <string>

class Scene;
class AssetManager;
class PhysicsSystem;

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
  };

  // Draws the full editor frame (menu bar, toolbar, panels, gizmo). `view` /
  // `proj` are the scene camera matrices; pass proj WITHOUT the Vulkan Y
  // flip — ImGuizmo handles screen mapping itself. Returns true when the
  // scene was modified this frame.
  bool draw(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);

  SelectionState selection;
  ToolbarState toolbar;

private:
  void drawMenuBar(Context &ctx);
  void drawHierarchy(Context &ctx);
  bool drawInspector(Context &ctx);
  void drawAssetsContent(Context &ctx);
  void drawLog();
  void drawStats(Context &ctx);
  void drawEnvironment(Context &ctx);
  bool drawGizmo(Context &ctx, const glm::mat4 &view, const glm::mat4 &proj);

  // Destroys an entity plus its Jolt body (the engine's render/physics
  // systems pick the removal up on their next update).
  void deleteEntity(Context &ctx, uint32_t id);
  void releasePhysicsBodies(Context &ctx);

  // Window visibility (same defaults as the old editor)
  bool mShowHierarchy = true;
  bool mShowInspector = true;
  bool mShowAssets = true;
  bool mShowEnvironment = true;
  bool mShowLog = true;
  bool mShowStats = true;

  // Console state
  bool mConsoleAutoScroll = true;
  bool mFilterInfo = true;
  bool mFilterWarn = true;
  bool mFilterError = true;
  char mConsoleSearch[128] = "";

  // Assets browser state
  char mAssetSearch[128] = "";
  std::string mBrowsePath;

  // FPS history for the stats graph
  static constexpr int kFpsHistorySize = 120;
  float mFpsHistory[kFpsHistorySize] = {};
  int mFpsHistoryIdx = 0;

  std::string mScenePath; // save/load target (defaults under assetDir)
};

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "AppState.h"
#include "CoreAppLayer.h"
#include "EditorSubsystem.h"
#include "RenderLoopSubsystem.h"

#include "ECS/Components.h"
#include "ECS/Systems/CameraSystem.h"
#include "ECS/Systems/RenderSystem.h"

#include "EngineEvents.h"
#include "GLStateCache.h"
#include "Keyboard.h"
#include "Logger.h"
#include "Mouse.h"
#include "MousePicking.h"
#include "Core/FrameProfiler.h"

#include "imgui.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "ImGuizmo.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
void saveConfig(const AppState &s, const char *filename) {}
void loadConfig(AppState &s, const char *filename) {}
constexpr float kTerrainEdgeMargin = 0.2f;
constexpr float kTerrainMinClearance = 0.15f;

bool isEntityAlive(Registry &reg, EntityId e) {
  if (!reg.has<LifecycleComponent>(e))
    return true;
  return reg.get<LifecycleComponent>(e).state == EntityLifecycleState::Alive;
}

float terrainClearanceForEntity(Registry &reg, EntityId e) {
  float clearance = kTerrainMinClearance;
  if (!reg.has<ColliderComponent>(e))
    return clearance;

  const auto &coll = reg.get<ColliderComponent>(e);
  switch (coll.shape) {
  case ColliderComponent::Shape::Box:
    clearance = std::max(clearance, coll.dimensions.y * 0.5f + 0.05f);
    break;
  case ColliderComponent::Shape::Sphere:
    clearance = std::max(clearance, coll.dimensions.x + 0.05f);
    break;
  case ColliderComponent::Shape::Capsule:
    clearance =
        std::max(clearance, coll.dimensions.y * 0.5f + coll.dimensions.x + 0.05f);
    break;
  }
  return clearance;
}

float terrainSupportRadiusForEntity(Registry &reg, EntityId e) {
  if (!reg.has<ColliderComponent>(e))
    return 0.35f;

  const auto &coll = reg.get<ColliderComponent>(e);
  switch (coll.shape) {
  case ColliderComponent::Shape::Box:
    return std::max(0.25f, std::max(coll.dimensions.x, coll.dimensions.z) * 0.5f);
  case ColliderComponent::Shape::Sphere:
    return std::max(0.25f, coll.dimensions.x);
  case ColliderComponent::Shape::Capsule:
    return std::max(0.25f, coll.dimensions.x);
  }
  return 0.35f;
}

float terrainSupportHeightForEntity(AppState &state, Registry &reg, EntityId e,
                                    float x, float z) {
  float h = state.terrainSystem.getHeightAt(x, z);
  const float r = terrainSupportRadiusForEntity(reg, e);
  if (r <= 0.0f)
    return h;

  const float diag = r * 0.70710678f;
  h = std::max(h, state.terrainSystem.getHeightAt(x + r, z));
  h = std::max(h, state.terrainSystem.getHeightAt(x - r, z));
  h = std::max(h, state.terrainSystem.getHeightAt(x, z + r));
  h = std::max(h, state.terrainSystem.getHeightAt(x, z - r));
  h = std::max(h, state.terrainSystem.getHeightAt(x + diag, z + diag));
  h = std::max(h, state.terrainSystem.getHeightAt(x - diag, z + diag));
  h = std::max(h, state.terrainSystem.getHeightAt(x + diag, z - diag));
  h = std::max(h, state.terrainSystem.getHeightAt(x - diag, z - diag));
  return h;
}

void clampEntityToTerrain(AppState &state, Registry &reg, EntityId e) {
  if (!reg.has<TransformComponent>(e) || !isEntityAlive(reg, e))
    return;

  auto &tr = reg.get<TransformComponent>(e);
  const glm::vec2 clampedXZ = state.terrainSystem.clampXZToLoadedRegion(
      tr.position.x, tr.position.z, kTerrainEdgeMargin);
  tr.position.x = clampedXZ.x;
  tr.position.z = clampedXZ.y;

  const float terrainY =
      terrainSupportHeightForEntity(state, reg, e, tr.position.x, tr.position.z);
  const float minY = terrainY + terrainClearanceForEntity(reg, e);
  if (tr.position.y < minY)
    tr.position.y = minY;
}
} // namespace

bool CoreAppLayer::initialize() {
  mState.events.subscribe<SaveConfigRequestedEvent>(
      [this](const SaveConfigRequestedEvent &) {
        mState.pending.requestSaveConfig = true;
      });
  mState.events.subscribe<LoadConfigRequestedEvent>(
      [this](const LoadConfigRequestedEvent &) {
        mState.pending.requestLoadConfig = true;
      });
  mState.events.subscribe<SaveProjectConfigRequestedEvent>(
      [this](const SaveProjectConfigRequestedEvent &) {
        mState.pending.requestSaveProjectConfig = true;
      });
  mState.events.subscribe<SpawnEntityRequestedEvent>(
      [this](const SpawnEntityRequestedEvent &e) {
        mState.pending.pendingSpawnPaths.push_back(e.path);
      });
  mState.events.subscribe<CreateEmptyEntityRequestedEvent>(
      [this](const CreateEmptyEntityRequestedEvent &e) {
        mState.pending.pendingEmptyEntityNames.push_back(e.name);
      });
  mState.events.subscribe<DeleteEntityRequestedEvent>(
      [this](const DeleteEntityRequestedEvent &e) {
        mState.pending.pendingDeleteEntityIds.push_back(e.entityId);
      });
  mState.events.subscribe<SaveSceneRequestedEvent>(
      [this](const SaveSceneRequestedEvent &e) {
        mState.pending.pendingSceneSavePath = e.path;
      });
  mState.events.subscribe<LoadSceneRequestedEvent>(
      [this](const LoadSceneRequestedEvent &e) {
        mState.pending.pendingSceneLoadPath = e.path;
      });
  mState.events.subscribe<UndoRequestedEvent>(
      [this](const UndoRequestedEvent &) {
        mState.history.requestUndo = true;
      });
  mState.events.subscribe<RedoRequestedEvent>(
      [this](const RedoRequestedEvent &) {
        mState.history.requestRedo = true;
      });
  mState.events.subscribe<SceneHistoryJumpRequestedEvent>(
      [this](const SceneHistoryJumpRequestedEvent &e) {
        mState.history.requestHistoryJump = e.index;
      });

  commitHistorySnapshot("Initial");

  return true;
}

void CoreAppLayer::shutdown() {}

void CoreAppLayer::applyHistorySnapshot(int idx) {
  if (idx < 0 || idx >= (int)mState.history.historySnapshots.size())
    return;
  if (!mState.scene.loadFromString(mState.history.historySnapshots[idx]))
    return;
  mState.history.historyCursor = idx;
  mState.selection.selectedEntityId = 0;
  mState.selection.selectedEntities.clear();
  mState.selection.lastClickedEntity = 0;
  mState.playerId = 0;
  for (auto e : mState.scene.registry().view<CameraComponent>()) {
    if (!mState.scene.registry().has<LifecycleComponent>(e) ||
        mState.scene.registry().get<LifecycleComponent>(e).state ==
            EntityLifecycleState::Alive) {
      mState.playerId = e;
      break;
    }
  }
}

void CoreAppLayer::commitHistorySnapshot(const std::string &label) {
  const std::string snap = mState.scene.serializeToString();
  if (mState.history.historyCursor >= 0 &&
      mState.history.historyCursor <
          (int)mState.history.historySnapshots.size() &&
      mState.history.historySnapshots[mState.history.historyCursor] == snap)
    return;

  if (mState.history.historyCursor + 1 <
      (int)mState.history.historySnapshots.size()) {
    mState.history.historySnapshots.erase(
        mState.history.historySnapshots.begin() + mState.history.historyCursor +
            1,
        mState.history.historySnapshots.end());
    mState.history.historyLabels.erase(mState.history.historyLabels.begin() +
                                           mState.history.historyCursor + 1,
                                       mState.history.historyLabels.end());
  }

  mState.history.historySnapshots.push_back(snap);
  mState.history.historyLabels.push_back(label);
  mState.history.historyCursor =
      (int)mState.history.historySnapshots.size() - 1;

  const int maxHistory = 128;
  if ((int)mState.history.historySnapshots.size() > maxHistory) {
    const int trim = (int)mState.history.historySnapshots.size() - maxHistory;
    mState.history.historySnapshots.erase(
        mState.history.historySnapshots.begin(),
        mState.history.historySnapshots.begin() + trim);
    mState.history.historyLabels.erase(mState.history.historyLabels.begin(),
                                       mState.history.historyLabels.begin() +
                                           trim);
    mState.history.historyCursor -= trim;
    if (mState.history.historyCursor < 0)
      mState.history.historyCursor = 0;
  }
}

void CoreAppLayer::update(float dt, float nowT) {
  // Always in editor mode — cursor always visible
  mState.uiMode = true;
  mState.profiler.beginFrame();
  mState.profiler.setFrameMs(dt * 1000.0f);
  GLStateCache::instance().resetCounters();

  if (mState.editorSubsystem)
    mState.editorSubsystem->beginFrame();

  // Tick the Laravel Network poll
  mState.networkSystem.update(dt, mState);

  float renderTime = mState.render.freezeTime ? mState.render.frozenTime : nowT;
  if (!mState.render.freezeTime)
    mState.render.frozenTime = nowT;

  EditorSelectionState selState = {mState.selection.selectedEntityId,
                                   mState.selection.selectedEntities,
                                   mState.selection.lastClickedEntity,
                                   mState.selection.editObjPart,
                                   mState.selection.selectedObjPartName,
                                   mState.selection.editColliderBounds,
                                   (int &)mState.selection.gizmoOp,
                                   (int &)mState.selection.gizmoMode,
                                   mState.selection.renaming,
                                   mState.selection.renameBuf,
                                   mState.selection.outlinerFilter,
                                   mState.selection.focusDistance};

  EditorContext ctx{
      mState.uiMode,
      mState.input.walkStep,
      mState.input.runMult,
      mState.input.jumpStrength,
      mState.input.gravity,
      mState.input.freezePhysics,
      mState.input.mouseSensitivity,
      mState.input.fov,
      mState.sun,
      mState.fire,
      mState.cloud,
      mState.sky,
      mState.projectiles,
      mState.postProcessor,
      mState.scene,
      mState.events,
      mState.projectConfig,
      mState.assets,
      mState.terrainSize,
      mState.terrainSpacing,
      mState.terrainSettings,
      mState.terrainMaterial,
      mState.terrainSystem,
      mState.editorCamera,
      mState.skyUI.solidSky,
      mState.skyUI.skyHorizon,
      mState.skyUI.skyTop,
      mState.render.shadowStrength,
      mState.render.shadowFarPlane,
      mState.render.exposure,
      mState.render.gamma,
      mState.render.fogDensity,
      mState.render.fogColor,
      mState.render.wireframe,
      mState.render.disableShadows,
      mState.render.disableClouds,
      mState.render.disableHDR,
      mState.render.freezeTime,
      dt,
      (int)mState.scene.registry().view<TransformComponent>().size(),
      (int)(mState.projectiles.count()),
      mState.renderSystem.stats().drawn,
      mState.renderSystem.stats().culled,
      mState.renderSystem.stats().drawCallsMain,
      mState.renderSystem.stats().drawCallsShadow,
      mState.renderSystem.stats().instancedDrawCallsMain,
      mState.renderSystem.stats().instancedDrawCallsShadow,
      mState.render.frustumCulling,
      &mState.lastRenderPassOrder,
      mState.hotReloadEnabled,
      mState.autoProcessImportQueue,
      &mState.hotReloadMessages,
      &mState.history.historyLabels,
      mState.history.historyCursor,
      &mState.profiler.samples(),
      mState.gpuFrameMs,
      mState.gpuShadowMs,
      mState.gpuMainMs,
      mState.glProgramBinds,
      mState.glTextureBinds,
      mState.glVaoBinds,
      mState.glStateChanges,
      selState,
      (int &)mState.playState};

  EditorUIOutput uiOut{};
  {
    ScopedCpuTimer timer(mState.profiler, "Editor UI");
    uiOut = mState.editor.draw(ctx);
  }
  if (uiOut.sceneModified) {
    mState.history.pendingHistoryCommit = true;
    mState.history.pendingHistoryLabel = "Edit Scene";
  }

  // Handle Play mode cursor locking and ESC to pause
  static AppState::PlayState lastPlayState = AppState::PlayState::Stopped;
  if (mState.playState == AppState::PlayState::Playing &&
      lastPlayState != AppState::PlayState::Playing) {
    glfwSetInputMode(mState.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  } else if (mState.playState != AppState::PlayState::Playing &&
             lastPlayState == AppState::PlayState::Playing) {
    glfwSetInputMode(mState.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }

  if (mState.playState == AppState::PlayState::Playing &&
      Keyboard::keyWentDown(GLFW_KEY_ESCAPE)) {
    mState.playState = AppState::PlayState::Paused;
    glfwSetInputMode(mState.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
  lastPlayState = mState.playState;

  bool sceneMutatedByCommands = false;
  {
    ScopedCpuTimer timer(mState.profiler, "Commands/Scene");
    if (mState.pending.requestSaveConfig) {
      saveConfig(mState, "editor_state.bin");
      mState.pending.requestSaveConfig = false;
    }
    if (mState.pending.requestLoadConfig) {
      loadConfig(mState, "editor_state.bin");
      mState.pending.requestLoadConfig = false;
    }
    if (mState.pending.requestSaveProjectConfig) {
      if (!mState.projectConfig.saveToFile("project_config.json")) {
        LOG_ERROR("Runtime", "Failed to save project_config.json");
      }
      mState.pending.requestSaveProjectConfig = false;
    }
    if (!mState.pending.pendingSceneSavePath.empty()) {
      if (!mState.scene.saveToFile(mState.pending.pendingSceneSavePath))
        LOG_ERROR(
            "Runtime",
            "Failed to save scene: " + mState.pending.pendingSceneSavePath);
      mState.pending.pendingSceneSavePath.clear();
    }
    if (!mState.pending.pendingSceneLoadPath.empty()) {
      if (!mState.scene.loadFromFile(mState.pending.pendingSceneLoadPath))
        LOG_ERROR(
            "Runtime",
            "Failed to load scene: " + mState.pending.pendingSceneLoadPath);
      else {
        mState.history.pendingHistoryCommit = true;
        mState.history.pendingHistoryLabel = "Load Scene";
        sceneMutatedByCommands = true;
        mState.playerId = 0;
        for (auto e : mState.scene.registry().view<CameraComponent>()) {
          if (!mState.scene.registry().has<LifecycleComponent>(e) ||
              mState.scene.registry().get<LifecycleComponent>(e).state ==
                  EntityLifecycleState::Alive) {
            mState.playerId = e;
            break;
          }
        }
      }
      mState.pending.pendingSceneLoadPath.clear();
    }

    if (mState.history.requestHistoryJump >= 0) {
      applyHistorySnapshot(mState.history.requestHistoryJump);
      mState.history.requestHistoryJump = -1;
      sceneMutatedByCommands = true;
    } else if (mState.history.requestUndo) {
      applyHistorySnapshot(mState.history.historyCursor - 1);
      sceneMutatedByCommands = true;
    } else if (mState.history.requestRedo) {
      applyHistorySnapshot(mState.history.historyCursor + 1);
      sceneMutatedByCommands = true;
    }
    mState.history.requestUndo = false;
    mState.history.requestRedo = false;

    if (mState.autoProcessImportQueue)
      mState.assets.processImportQueue();
    if (mState.hotReloadEnabled) {
      mState.hotReloadMessages = mState.assets.pollHotReload();
    } else {
      mState.hotReloadMessages.clear();
    }

    for (const std::string &emptyName :
         mState.pending.pendingEmptyEntityNames) {
      (void)mState.scene.createEmptyEntity(emptyName.empty() ? "Empty"
                                                             : emptyName);
      mState.history.pendingHistoryCommit = true;
      mState.history.pendingHistoryLabel = "Create Entity";
      sceneMutatedByCommands = true;
    }
    mState.pending.pendingEmptyEntityNames.clear();

    for (uint32_t entityId : mState.pending.pendingDeleteEntityIds) {
      if (entityId != 0) {
        mState.scene.deleteEntity(entityId);
        mState.history.pendingHistoryCommit = true;
        mState.history.pendingHistoryLabel = "Delete Entity";
        sceneMutatedByCommands = true;
      }
    }
    mState.pending.pendingDeleteEntityIds.clear();

    for (const std::string &path : mState.pending.pendingSpawnPaths) {
      uint32_t spawnedId = 0;

      // Intercept procedural primitives (__primitive_cube, etc.)
      const std::string prefix = "__primitive_";
      if (path.substr(0, prefix.size()) == prefix) {
        std::string shape = path.substr(prefix.size());
        spawnedId = mState.scene.spawnPrimitive(shape);
      } else {
        spawnedId = mState.scene.spawnFromFile(path);
      }

      if (spawnedId == 0) {
        LOG_ERROR("Runtime", "Failed to spawn: " + path);
      } else {
        mState.history.pendingHistoryCommit = true;
        mState.history.pendingHistoryLabel = "Spawn Asset";
        sceneMutatedByCommands = true;
      }
    }
    mState.pending.pendingSpawnPaths.clear();

    if (!mState.pending.pendingDropPaths.empty()) {
      for (const std::string &path : mState.pending.pendingDropPaths) {
        const uint32_t spawnedId = mState.scene.spawnFromFile(path);
        if (spawnedId == 0)
          LOG_ERROR("Runtime", "Failed to load dropped model: " + path);
        else {
          mState.history.pendingHistoryCommit = true;
          mState.history.pendingHistoryLabel = "Spawn Asset";
          sceneMutatedByCommands = true;
        }
      }
      mState.pending.pendingDropPaths.clear();
    }

    const size_t entityCountBeforeFlush =
        mState.scene.registry().view<TransformComponent>().size();
    mState.scene.flushPendingDestroy();
    const size_t entityCountAfterFlush =
        mState.scene.registry().view<TransformComponent>().size();
    if (entityCountAfterFlush != entityCountBeforeFlush) {
      mState.history.pendingHistoryCommit = true;
      mState.history.pendingHistoryLabel = "Destroy Entity";
      sceneMutatedByCommands = true;
    }

    // Keep raw mesh pointers synchronized with authoritative asset handles.
    mState.scene.refreshMeshReferences();
  }

  if (mState.render.wireframe)
    GLStateCache::instance().setPolygonMode(GL_LINE);
  else
    GLStateCache::instance().setPolygonMode(GL_FILL);

  // Run Lua scripts only when Playing
  if (mState.playState == AppState::PlayState::Playing) {
    {
      ScopedCpuTimer timer(mState.profiler, "Scripts");
      mState.scriptSystem.update(mState.scene.registry(), dt);
    }
    {
      ScopedCpuTimer timer(mState.profiler, "Physics");
      mState.physicsSystem.update(mState.scene.registry(), dt);
    }

    if (mState.terrainSystem.isEnabled()) {
      ScopedCpuTimer timer(mState.profiler, "Terrain Clamp");
      auto &reg = mState.scene.registry();
      for (auto e : reg.viewAll<TransformComponent, ScriptComponent>())
        clampEntityToTerrain(mState, reg, e);
    }
  }

  // ── Editor Camera (orbit / pan / zoom via mouse) ──
  {
    ScopedCpuTimer timer(mState.profiler, "Editor Camera");
    mState.editorCamera.update(mState.window,
                               uiOut.wantCaptureMouse || ImGuizmo::IsUsing());
  }

  // F key: focus on selected entity
  if (Keyboard::key(GLFW_KEY_F) && !uiOut.wantCaptureKeyboard &&
      mState.selection.selectedEntityId != 0) {
    auto &reg = mState.scene.registry();
    if (reg.has<TransformComponent>(mState.selection.selectedEntityId)) {
      glm::vec3 target =
          reg.get<TransformComponent>(mState.selection.selectedEntityId)
              .position;
      mState.editorCamera.focusOn(target);
    }
  }

  glm::vec3 cameraPos = mState.editorCamera.getPosition();
  glm::vec3 cameraFront = mState.editorCamera.getForwardVector();
  glm::vec3 cameraUp = mState.editorCamera.getUpVector();

  glm::mat4 view = mState.editorCamera.getViewMatrix();

  if (mState.playState == AppState::PlayState::Playing) {
    auto &reg = mState.scene.registry();
    if (mState.playerId == 0 || !reg.has<CameraComponent>(mState.playerId)) {
      mState.playerId =
          0; // Reset — stays 0 if no CameraComponent entity exists
      for (auto e : reg.view<CameraComponent>()) {
        if (!reg.has<LifecycleComponent>(e) ||
            reg.get<LifecycleComponent>(e).state ==
                EntityLifecycleState::Alive) {
          mState.playerId = e;
          break;
        }
      }
    }

    if (mState.playerId != 0 && reg.has<TransformComponent>(mState.playerId)) {
      if (mState.terrainSystem.isEnabled())
        clampEntityToTerrain(mState, reg, mState.playerId);
      auto &tr = reg.get<TransformComponent>(mState.playerId);

      cameraPos = tr.position;

      glm::vec3 front;
      front.x =
          -sin(glm::radians(tr.rotation.y)) * cos(glm::radians(tr.rotation.x));
      front.y = sin(glm::radians(tr.rotation.x));
      front.z =
          -cos(glm::radians(tr.rotation.y)) * cos(glm::radians(tr.rotation.x));
      cameraFront = glm::normalize(front);

      glm::vec3 right =
          glm::normalize(glm::cross(cameraFront, glm::vec3(0.0f, 1.0f, 0.0f)));
      cameraUp = glm::normalize(glm::cross(right, cameraFront));

      view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
    }
  }

  // Update Terrain chunk loading around the camera
  {
    ScopedCpuTimer timer(mState.profiler, "Terrain Update");
    mState.terrainSystem.update(cameraPos);
  }

  int winW, winH;
  glfwGetWindowSize(mState.window, &winW, &winH);
  glm::mat4 projection = glm::perspective(
      glm::radians(mState.input.fov), (float)winW / (float)winH, 0.1f, 500.0f);
  const bool allowTemporalJitter =
      (mState.playState == AppState::PlayState::Playing);
  if (!allowTemporalJitter)
    mState.postProcessor.resetTemporalHistory();
  glm::mat4 renderProjection =
      mState.postProcessor.jitteredProjection(projection, allowTemporalJitter);

  if (mState.uiMode && Mouse::buttonWentDown(GLFW_MOUSE_BUTTON_LEFT) &&
      !uiOut.wantCaptureMouse && !ImGuizmo::IsUsing()) {
    float mx = (float)Mouse::getMouseX();
    float my = (float)Mouse::getMouseY();
    Ray ray = MousePicking::screenToRay(mx, my, 0.0f, 0.0f, (float)winW,
                                        (float)winH, view, projection);
    uint32_t hitId = MousePicking::pickEntity(ray, mState.scene.registry());

    if (hitId != 0) {
      bool ctrlHeld =
          glfwGetKey(mState.window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
          glfwGetKey(mState.window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS ||
          glfwGetKey(mState.window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS;
      if (ctrlHeld) {
        auto &sel = mState.selection.selectedEntities;
        auto it = std::find(sel.begin(), sel.end(), hitId);
        if (it != sel.end()) {
          sel.erase(it);
          if (mState.selection.selectedEntityId == hitId)
            mState.selection.selectedEntityId = sel.empty() ? 0 : sel.back();
        } else {
          sel.push_back(hitId);
          mState.selection.selectedEntityId = hitId;
        }
      } else {
        mState.selection.selectedEntities.clear();
        mState.selection.selectedEntities.push_back(hitId);
        mState.selection.selectedEntityId = hitId;
      }
      mState.selection.lastClickedEntity = hitId;

      auto &reg = mState.scene.registry();
      if (reg.has<MeshComponent>(hitId) && reg.has<TransformComponent>(hitId)) {
        OBJModel *mdl = reg.get<MeshComponent>(hitId).objModel;
        if (mdl && mdl->submeshCount() > 1) {
          auto &tr = reg.get<TransformComponent>(hitId);
          std::string hitPart = MousePicking::pickSubmesh(ray, tr, *mdl);
          if (!hitPart.empty()) {
            mState.selection.editObjPart = true;
            mState.selection.selectedObjPartName = hitPart;
          } else {
            mState.selection.editObjPart = false;
            mState.selection.selectedObjPartName.clear();
          }
        } else {
          mState.selection.editObjPart = false;
          mState.selection.selectedObjPartName.clear();
        }
      } else {
        mState.selection.editObjPart = false;
        mState.selection.selectedObjPartName.clear();
      }
    } else {
      mState.selection.selectedEntities.clear();
      mState.selection.selectedEntityId = 0;
      mState.selection.editObjPart = false;
      mState.selection.selectedObjPartName.clear();
    }
  }

  {
    if (mState.editor.drawGizmo(mState.uiMode, view, projection, mState.scene,
                                mState.sun, mState.events, selState,
                                cameraPos)) {
      mState.history.pendingHistoryCommit = true;
      mState.history.pendingHistoryLabel = "Edit Scene";
    }
  }

  if (mState.history.pendingHistoryCommit) {
    const bool interacting = ImGuizmo::IsUsing() || ImGui::IsAnyItemActive() ||
                             ImGui::IsMouseDown(0);
    if (!interacting || sceneMutatedByCommands) {
      commitHistorySnapshot(mState.history.pendingHistoryLabel.empty()
                                ? "Edit Scene"
                                : mState.history.pendingHistoryLabel);
      mState.history.pendingHistoryCommit = false;
      mState.history.pendingHistoryLabel.clear();
    }
  }

  {
    ScopedCpuTimer timer(mState.profiler, "Projectiles");
    mState.projectiles.update(dt);
  }
  mState.renderSystem.setViewProjection(renderProjection * view);
  mState.renderSystem.setCameraPosition(cameraPos);
  mState.renderSystem.setCullingEnabled(mState.render.frustumCulling);
  mState.renderSystem.beginFrame();

  if (mState.renderLoopSubsystem) {
    ScopedCpuTimer timer(mState.profiler, "Render CPU");
    mState.renderLoopSubsystem->executeRenderPasses(
        view, renderProjection, cameraPos, cameraFront, cameraUp, renderTime);
  }

  if (mState.editorSubsystem) {
    ScopedCpuTimer timer(mState.profiler, "ImGui End");
    mState.editorSubsystem->endFrame();
  }

  const auto glStats = GLStateCache::instance().counters();
  mState.glProgramBinds = glStats.programBinds;
  mState.glTextureBinds = glStats.textureBinds;
  mState.glVaoBinds = glStats.vaoBinds;
  mState.glStateChanges = glStats.stateChanges;

  mState.profiler.endFrame();
}

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "AppState.h"
#include "CoreAppLayer.h"
#include "EditorSubsystem.h"
#include "RenderLoopSubsystem.h"

#include "ECS/Components.h"
#include "ECS/Systems/CameraSystem.h"
#include "ECS/Systems/MovementSystem.h"
#include "ECS/Systems/RenderSystem.h"

#include "EngineEvents.h"
#include "GLStateCache.h"
#include "Keyboard.h"
#include "Logger.h"
#include "Mouse.h"
#include "MousePicking.h"

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
} // namespace

bool CoreAppLayer::initialize() {
  mState.events.subscribe<SaveConfigRequestedEvent>(
      [this](const SaveConfigRequestedEvent &) {
        mState.requestSaveConfig = true;
      });
  mState.events.subscribe<LoadConfigRequestedEvent>(
      [this](const LoadConfigRequestedEvent &) {
        mState.requestLoadConfig = true;
      });
  mState.events.subscribe<SaveProjectConfigRequestedEvent>(
      [this](const SaveProjectConfigRequestedEvent &) {
        mState.requestSaveProjectConfig = true;
      });
  mState.events.subscribe<SpawnEntityRequestedEvent>(
      [this](const SpawnEntityRequestedEvent &e) {
        mState.pendingSpawnPaths.push_back(e.path);
      });
  mState.events.subscribe<CreateEmptyEntityRequestedEvent>(
      [this](const CreateEmptyEntityRequestedEvent &e) {
        mState.pendingEmptyEntityNames.push_back(e.name);
      });
  mState.events.subscribe<DeleteEntityRequestedEvent>(
      [this](const DeleteEntityRequestedEvent &e) {
        mState.pendingDeleteEntityIds.push_back(e.entityId);
      });
  mState.events.subscribe<SaveSceneRequestedEvent>(
      [this](const SaveSceneRequestedEvent &e) {
        mState.pendingSceneSavePath = e.path;
      });
  mState.events.subscribe<LoadSceneRequestedEvent>(
      [this](const LoadSceneRequestedEvent &e) {
        mState.pendingSceneLoadPath = e.path;
      });
  mState.events.subscribe<UndoRequestedEvent>(
      [this](const UndoRequestedEvent &) { mState.requestUndo = true; });
  mState.events.subscribe<RedoRequestedEvent>(
      [this](const RedoRequestedEvent &) { mState.requestRedo = true; });
  mState.events.subscribe<SceneHistoryJumpRequestedEvent>(
      [this](const SceneHistoryJumpRequestedEvent &e) {
        mState.requestHistoryJump = e.index;
      });

  commitHistorySnapshot("Initial");
  return true;
}

void CoreAppLayer::shutdown() {}

void CoreAppLayer::applyHistorySnapshot(int idx) {
  if (idx < 0 || idx >= (int)mState.historySnapshots.size())
    return;
  if (!mState.scene.loadFromString(mState.historySnapshots[idx]))
    return;
  mState.historyCursor = idx;
  mState.selectedEntityId = 0;
  mState.selectedEntities.clear();
  mState.lastClickedEntity = 0;
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
  if (mState.historyCursor >= 0 &&
      mState.historyCursor < (int)mState.historySnapshots.size() &&
      mState.historySnapshots[mState.historyCursor] == snap)
    return;

  if (mState.historyCursor + 1 < (int)mState.historySnapshots.size()) {
    mState.historySnapshots.erase(mState.historySnapshots.begin() +
                                      mState.historyCursor + 1,
                                  mState.historySnapshots.end());
    mState.historyLabels.erase(mState.historyLabels.begin() +
                                   mState.historyCursor + 1,
                               mState.historyLabels.end());
  }

  mState.historySnapshots.push_back(snap);
  mState.historyLabels.push_back(label);
  mState.historyCursor = (int)mState.historySnapshots.size() - 1;

  const int maxHistory = 128;
  if ((int)mState.historySnapshots.size() > maxHistory) {
    const int trim = (int)mState.historySnapshots.size() - maxHistory;
    mState.historySnapshots.erase(mState.historySnapshots.begin(),
                                  mState.historySnapshots.begin() + trim);
    mState.historyLabels.erase(mState.historyLabels.begin(),
                               mState.historyLabels.begin() + trim);
    mState.historyCursor -= trim;
    if (mState.historyCursor < 0)
      mState.historyCursor = 0;
  }
}

void CoreAppLayer::update(float dt, float nowT) {
  bool escDown = Keyboard::key(GLFW_KEY_ESCAPE);
  if (escDown && !mState.escWasDown) {
    mState.uiMode = !mState.uiMode;
    glfwSetInputMode(mState.window, GLFW_CURSOR,
                     mState.uiMode ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
  }
  mState.escWasDown = escDown;

  if (mState.editorSubsystem)
    mState.editorSubsystem->beginFrame();

  float renderTime = mState.freezeTime ? mState.frozenTime : nowT;
  if (!mState.freezeTime)
    mState.frozenTime = nowT;

  EditorSelectionState selState{
      mState.selectedEntityId,    mState.selectedEntities,
      mState.lastClickedEntity,   mState.editObjPart,
      mState.selectedObjPartName, (int &)mState.gizmoOp,
      (int &)mState.gizmoMode,    mState.renaming,
      mState.renameBuf,           mState.outlinerFilter,
      mState.focusDistance};

  EditorContext ctx{
      mState.uiMode,
      mState.walkStep,
      mState.runMult,
      mState.jumpStrength,
      mState.gravity,
      mState.freezePhysics,
      mState.mouseSensitivity,
      mState.fov,
      mState.sun,
      mState.fire,
      mState.cloud,
      mState.sky,
      mState.projectiles,
      mState.scene,
      mState.events,
      mState.projectConfig,
      mState.assets,
      mState.terrainSize,
      mState.terrainSpacing,
      mState.solidSky,
      mState.skyHorizon,
      mState.skyTop,
      mState.shadowStrength,
      mState.shadowFarPlane,
      mState.exposure,
      mState.gamma,
      mState.wireframe,
      mState.disableShadows,
      mState.disableClouds,
      mState.disableHDR,
      mState.freezeTime,
      dt,
      (int)mState.scene.registry().view<TransformComponent>().size(),
      (int)(mState.projectiles.count()),
      mState.renderSystem.stats().drawn,
      mState.renderSystem.stats().culled,
      mState.frustumCulling,
      &mState.lastRenderPassOrder,
      mState.hotReloadEnabled,
      mState.autoProcessImportQueue,
      &mState.hotReloadMessages,
      &mState.historyLabels,
      mState.historyCursor,
      selState};

  EditorUIOutput uiOut = mState.editor.draw(ctx);
  if (uiOut.sceneModified) {
    mState.pendingHistoryCommit = true;
    mState.pendingHistoryLabel = "Edit Scene";
  }
  bool sceneMutatedByCommands = false;
  if (mState.requestSaveConfig) {
    saveConfig(mState, "editor_state.bin");
    mState.requestSaveConfig = false;
  }
  if (mState.requestLoadConfig) {
    loadConfig(mState, "editor_state.bin");
    mState.requestLoadConfig = false;
  }
  if (mState.requestSaveProjectConfig) {
    if (!mState.projectConfig.saveToFile("project_config.json")) {
      LOG_ERROR("Runtime", "Failed to save project_config.json");
    }
    mState.requestSaveProjectConfig = false;
  }
  if (!mState.pendingSceneSavePath.empty()) {
    if (!mState.scene.saveToFile(mState.pendingSceneSavePath))
      LOG_ERROR("Runtime",
                "Failed to save scene: " + mState.pendingSceneSavePath);
    mState.pendingSceneSavePath.clear();
  }
  if (!mState.pendingSceneLoadPath.empty()) {
    if (!mState.scene.loadFromFile(mState.pendingSceneLoadPath))
      LOG_ERROR("Runtime",
                "Failed to load scene: " + mState.pendingSceneLoadPath);
    else {
      mState.pendingHistoryCommit = true;
      mState.pendingHistoryLabel = "Load Scene";
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
    mState.pendingSceneLoadPath.clear();
  }

  if (mState.requestHistoryJump >= 0) {
    applyHistorySnapshot(mState.requestHistoryJump);
    mState.requestHistoryJump = -1;
    sceneMutatedByCommands = true;
  } else if (mState.requestUndo) {
    applyHistorySnapshot(mState.historyCursor - 1);
    sceneMutatedByCommands = true;
  } else if (mState.requestRedo) {
    applyHistorySnapshot(mState.historyCursor + 1);
    sceneMutatedByCommands = true;
  }
  mState.requestUndo = false;
  mState.requestRedo = false;

  if (mState.autoProcessImportQueue)
    mState.assets.processImportQueue();
  if (mState.hotReloadEnabled) {
    mState.hotReloadMessages = mState.assets.pollHotReload();
  } else {
    mState.hotReloadMessages.clear();
  }

  for (const std::string &emptyName : mState.pendingEmptyEntityNames) {
    (void)mState.scene.createEmptyEntity(emptyName.empty() ? "Empty"
                                                           : emptyName);
    mState.pendingHistoryCommit = true;
    mState.pendingHistoryLabel = "Create Entity";
    sceneMutatedByCommands = true;
  }
  mState.pendingEmptyEntityNames.clear();

  for (uint32_t entityId : mState.pendingDeleteEntityIds) {
    if (entityId != 0) {
      mState.scene.deleteEntity(entityId);
      mState.pendingHistoryCommit = true;
      mState.pendingHistoryLabel = "Delete Entity";
      sceneMutatedByCommands = true;
    }
  }
  mState.pendingDeleteEntityIds.clear();

  for (const std::string &path : mState.pendingSpawnPaths) {
    const uint32_t spawnedId = mState.scene.spawnFromFile(path);
    if (spawnedId == 0) {
      LOG_ERROR("Runtime", "Failed to load model from path: " + path);
    } else {
      mState.pendingHistoryCommit = true;
      mState.pendingHistoryLabel = "Spawn Asset";
      sceneMutatedByCommands = true;
    }
  }
  mState.pendingSpawnPaths.clear();

  if (!mState.pendingDropPaths.empty()) {
    for (const std::string &path : mState.pendingDropPaths) {
      const uint32_t spawnedId = mState.scene.spawnFromFile(path);
      if (spawnedId == 0)
        LOG_ERROR("Runtime", "Failed to load dropped model: " + path);
      else {
        mState.pendingHistoryCommit = true;
        mState.pendingHistoryLabel = "Spawn Asset";
        sceneMutatedByCommands = true;
      }
    }
    mState.pendingDropPaths.clear();
  }

  const size_t entityCountBeforeFlush =
      mState.scene.registry().view<TransformComponent>().size();
  mState.scene.flushPendingDestroy();
  const size_t entityCountAfterFlush =
      mState.scene.registry().view<TransformComponent>().size();
  if (entityCountAfterFlush != entityCountBeforeFlush) {
    mState.pendingHistoryCommit = true;
    mState.pendingHistoryLabel = "Destroy Entity";
    sceneMutatedByCommands = true;
  }

  if (mState.wireframe)
    GLStateCache::instance().setPolygonMode(GL_LINE);
  else
    GLStateCache::instance().setPolygonMode(GL_FILL);

  bool allowGameInput = (!mState.uiMode && !uiOut.wantCaptureKeyboard);

  if (allowGameInput) {
    mState.movementSystem.update(mState.scene.registry(), dt, mState.walkStep,
                                 mState.runMult, mState.jumpStrength);
  }

  bool allowMouseLook =
      (!mState.uiMode && !uiOut.wantCaptureMouse && !ImGuizmo::IsUsing());
  if (allowMouseLook) {
    mState.cameraSystem.update(mState.scene.registry(),
                               mState.mouseSensitivity);
  } else {
    Mouse::getDX(); // Consume delta
    Mouse::getDY();
  }

  glm::vec3 cameraPos(0, 0, 3);
  glm::vec3 cameraFront(0, 0, -1);
  glm::vec3 cameraUp(0, 1, 0);

  if (mState.playerId != 0) {
    if (mState.scene.registry().has<TransformComponent>(mState.playerId)) {
      cameraPos = mState.scene.registry()
                      .get<TransformComponent>(mState.playerId)
                      .position;
    }
    if (mState.scene.registry().has<CameraComponent>(mState.playerId)) {
      const auto &cam =
          mState.scene.registry().get<CameraComponent>(mState.playerId);
      cameraFront = cam.front;
      cameraUp = cam.up;
    }
  }

  glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);

  int winW, winH;
  glfwGetWindowSize(mState.window, &winW, &winH);
  glm::mat4 projection = glm::perspective(
      glm::radians(mState.fov), (float)winW / (float)winH, 0.1f, 500.0f);

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
        auto &sel = mState.selectedEntities;
        auto it = std::find(sel.begin(), sel.end(), hitId);
        if (it != sel.end()) {
          sel.erase(it);
          if (mState.selectedEntityId == hitId)
            mState.selectedEntityId = sel.empty() ? 0 : sel.back();
        } else {
          sel.push_back(hitId);
          mState.selectedEntityId = hitId;
        }
      } else {
        mState.selectedEntities.clear();
        mState.selectedEntities.push_back(hitId);
        mState.selectedEntityId = hitId;
      }
      mState.lastClickedEntity = hitId;

      auto &reg = mState.scene.registry();
      if (reg.has<MeshComponent>(hitId) && reg.has<TransformComponent>(hitId)) {
        OBJModel *mdl = reg.get<MeshComponent>(hitId).objModel;
        if (mdl && mdl->submeshCount() > 1) {
          auto &tr = reg.get<TransformComponent>(hitId);
          std::string hitPart = MousePicking::pickSubmesh(ray, tr, *mdl);
          if (!hitPart.empty()) {
            mState.editObjPart = true;
            mState.selectedObjPartName = hitPart;
          } else {
            mState.editObjPart = false;
            mState.selectedObjPartName.clear();
          }
        } else {
          mState.editObjPart = false;
          mState.selectedObjPartName.clear();
        }
      } else {
        mState.editObjPart = false;
        mState.selectedObjPartName.clear();
      }
    } else {
      mState.selectedEntities.clear();
      mState.selectedEntityId = 0;
      mState.editObjPart = false;
      mState.selectedObjPartName.clear();
    }
  }

  {
    if (mState.editor.drawGizmo(mState.uiMode, view, projection, mState.scene,
                                mState.sun, mState.events, selState,
                                cameraPos)) {
      mState.pendingHistoryCommit = true;
      mState.pendingHistoryLabel = "Edit Scene";
    }
  }

  if (mState.pendingHistoryCommit) {
    const bool interacting = ImGuizmo::IsUsing() || ImGui::IsAnyItemActive() ||
                             ImGui::IsMouseDown(0);
    if (!interacting || sceneMutatedByCommands) {
      commitHistorySnapshot(mState.pendingHistoryLabel.empty()
                                ? "Edit Scene"
                                : mState.pendingHistoryLabel);
      mState.pendingHistoryCommit = false;
      mState.pendingHistoryLabel.clear();
    }
  }

  bool editingSun =
      (mState.uiMode && mState.selectedEntityId == 0 && ImGuizmo::IsUsing());
  if (!editingSun)
    mState.sun.update(dt, renderTime);

  mState.projectiles.update(dt);
  mState.renderSystem.setViewProjection(projection * view);
  mState.renderSystem.setCameraPosition(cameraPos);
  mState.renderSystem.setCullingEnabled(mState.frustumCulling);

  if (mState.renderLoopSubsystem) {
    mState.renderLoopSubsystem->executeRenderPasses(
        view, projection, cameraPos, cameraFront, cameraUp, renderTime);
  }

  if (mState.editorSubsystem)
    mState.editorSubsystem->endFrame();
}

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "App.h"
#include "EngineEvents.h"
#include "EventBus.h"
#include "IEngineSubsystem.h"
#include "ProjectConfig.h"
#include "SubsystemManager.h"
#include "CloudFX.h"
#include "AssetManager.h"
#include "CrashHandler.h"
#include "ECS/Components.h"
#include "ECS/Systems/CameraSystem.h"
#include "ECS/Systems/MovementSystem.h"
#include "ECS/Systems/RenderSystem.h"
#include "EditorUI.h"
#include "FBXModel.h"
#include "FireFX.h"
#include "HDRSky.h"
#include "Keyboard.h"
#include "GLStateCache.h"
#include "GLDebug.h"
#include "Mouse.h"
#include "OBJModel.h"
#include "ProjectileSystem.h"
#include "RenderGraph.h"
#include "Renderer.h"
#include "Logger.h"
#include "Scene.h"
#include "Shader.h"
#include "SunFX.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "ImGuizmo.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <string>
#include <vector>

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
  EditorUI editor;
  ProjectileSystem projectiles;

  // ECS Systems
  RenderSystem renderSystem;
  MovementSystem movementSystem;
  CameraSystem cameraSystem;
  RenderGraph renderGraph;
  std::vector<std::string> lastRenderPassOrder;

  // Player ID
  uint32_t playerId = 0;

  // Terrain/editor vars
  int terrainSize = 10;
  float terrainSpacing = 1.0f;

  // Fire
  bool hasFire = false;

  // Sub-object selection
  std::string selectedObjPartName;
  bool editObjPart = false;

  // Tuning
  float mixVal = 0.5f;
  float walkStep = 0.03f;
  float runMult = 2.0f;
  float jumpStrength = 0.18f;
  float gravity = 0.01f;
  float fov = 50.0f;
  bool freezePhysics = false;
  float mouseSensitivity = 0.10f;

  // Renderer controls
  float shadowStrength = 1.5f;
  float shadowFarPlane = 250.0f;
  float exposure = 1.0f;
  float gamma = 2.2f;

  // Render debug toggles
  bool wireframe = false;
  bool disableShadows = false;
  bool disableClouds = false;
  bool freezeTime = false;
  float frozenTime = 0.0f;
  bool frustumCulling = true;

  // UI mode
  bool uiMode = false;
  bool escWasDown = false;

  // Outliner & Selection
  char outlinerFilter[128] = "";
  std::vector<uint32_t> selectedEntities;
  uint32_t lastClickedEntity = 0;
  bool renaming = false;
  char renameBuf[128] = "";
  float focusDistance = 12.0f;
  uint32_t selectedEntityId = 0;

  ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;
  ImGuizmo::MODE gizmoMode = ImGuizmo::WORLD;

  // Sky UI
  bool solidSky = true;
  float skyHorizon[3] = {0.70f, 0.80f, 0.95f};
  float skyTop[3] = {0.12f, 0.20f, 0.45f};

  // Pending model import paths from OS drag-drop callback
  std::vector<std::string> pendingDropPaths;
  std::vector<std::string> pendingSpawnPaths;
  std::vector<uint32_t> pendingDeleteEntityIds;
  std::vector<std::string> pendingEmptyEntityNames;

  bool requestSaveConfig = false;
  bool requestLoadConfig = false;
  bool requestSaveProjectConfig = false;
  std::string pendingSceneSavePath;
  std::string pendingSceneLoadPath;

  ProjectConfig projectConfig;
  EventBus events;
  SubsystemManager subsystems;
  AssetManager assets;

  std::vector<std::string> hotReloadMessages;
  bool hotReloadEnabled = true;
  bool autoProcessImportQueue = false;

  bool requestUndo = false;
  bool requestRedo = false;
  int requestHistoryJump = -1;
  std::vector<std::string> historySnapshots;
  std::vector<std::string> historyLabels;
  int historyCursor = -1;
  bool pendingHistoryCommit = false;
  std::string pendingHistoryLabel;
};

namespace {

void initImGui(GLFWwindow *window) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
#ifdef IMGUI_HAS_DOCK
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
  io.IniFilename = "imgui.ini";
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");
}

void shutdownImGui() {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
}

void beginImGuiFrame() {
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void drawDockspace() {
#ifdef IMGUI_HAS_DOCK
  ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
  ImGui::DockSpaceOverViewport(ImGui::GetMainViewport(), dockspaceFlags);
#endif
}

void endImGuiFrame() {
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

#include <fstream>

void saveConfig(const AppState &s, const char *filename) {
  // TODO: Port to ECS
}

void loadConfig(AppState &s, const char *filename) {
  // TODO: Port to ECS
}

// drawSkyUI removed — merged into EditorUI::draw() via EditorContext

void renderShadowPass(AppState &s, const glm::vec3 &lightPos, float nearPlane,
                      float farPlane) {
  glm::mat4 shadowProj =
      glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, farPlane);

  glm::mat4 shadowMats[6] = {
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(1, 0, 0),
                               glm::vec3(0, -1, 0)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(-1, 0, 0),
                               glm::vec3(0, -1, 0)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, 1, 0),
                               glm::vec3(0, 0, 1)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, -1, 0),
                               glm::vec3(0, 0, -1)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, 0, 1),
                               glm::vec3(0, -1, 0)),
      shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0, 0, -1),
                               glm::vec3(0, -1, 0))};

  s.renderer.beginShadowPass();

  for (int face = 0; face < 6; ++face) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                           s.renderer.shadowCubeTex(), 0);

    glClear(GL_DEPTH_BUFFER_BIT);

    Shader &depthSh = s.renderer.shadowShader();
    depthSh.activate();
    depthSh.setMat4("shadowMatrix", shadowMats[face]);
    depthSh.setVec3("lightPos", lightPos);
    depthSh.setFloat("far_plane", farPlane);

    // Legacy drawAllDepth removed — RenderSystem handles all ECS entities
    s.renderSystem.update(s.scene.registry(), depthSh, true);
  }

  s.renderer.endShadowPass();
}

void renderMainPass(AppState &s, const glm::mat4 &view,
                    const glm::mat4 &projection, const glm::vec3 &cameraPos,
                    const glm::vec3 &cameraFront, const glm::vec3 &cameraUp,
                    float nowT) {
  glm::vec3 lightPos = s.sun.sunPos;
  float far_plane = 250.0f;

  s.renderer.beginFrame(0.2f, 0.3f, 0.3f, 1.0f);
  s.sky.draw(view, projection, s.exposure, s.gamma);

  // 1. CLEANUP: Unbind the Skybox's HDR texture from Unit 0
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  // 2. ACTIVATE SHADER BEFORE SETTING UNIFORMS
  s.renderer.shader().activate();

  // 3. CRITICAL FIX: Explicitly set Sampler Indices
  // Tell the shader: "texture1 is on Unit 0, shadowCube is on Unit 1"
  s.renderer.shader().setInt("texture1", 0);
  s.renderer.shader().setInt("shadowCube", 1);

  s.renderer.setFrameUniforms(
      view, projection, s.mixVal, nowT, s.sun.sunColor, s.sun.ambientStrength,
      cameraPos, s.sun.glowStrength, lightPos, far_plane, s.shadowStrength);

  // ... (fire code) ...
  s.renderer.shader().setBool("uHasFire", false);

  // ECS Rendering
  s.renderSystem.update(s.scene.registry(), s.renderer.shader(), false);

  s.projectiles.draw(view, projection, 0.25f);

  s.renderer.shader().activate();
  s.sun.draw(s.renderer.shader(), cameraFront, cameraUp);
}

bool initWindowAndImGui(AppState &s) {
  glfwSetErrorCallback(App::errorCallback);
  if (!glfwInit())
    return false;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

  GLFWmonitor *mon = glfwGetPrimaryMonitor();
  const GLFWvidmode *mode = glfwGetVideoMode(mon);
  s.scrW = mode->width;
  s.scrH = mode->height;

  s.window = glfwCreateWindow(s.scrW, s.scrH, "glGen", nullptr, nullptr);
  if (!s.window)
    return false;

  int mx, my;
  glfwGetMonitorPos(mon, &mx, &my);
  glfwSetWindowPos(s.window, mx, my);

  glfwMakeContextCurrent(s.window);
  glfwSetInputMode(s.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  glfwSetWindowUserPointer(s.window, &s);

  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    LOG_FATAL("Runtime", "Failed to initialize OpenGL libraries");
    return false;
  }

  glViewport(0, 0, s.scrW, s.scrH);
  glfwSetFramebufferSizeCallback(s.window, App::framebufferSizeCallback);
  glfwSetKeyCallback(s.window, Keyboard::keyCallBack);
  glfwSetCursorPosCallback(s.window, Mouse::cursorPosCallback);
  glfwSetMouseButtonCallback(s.window, Mouse::mouseButtonCallback);
  glfwSetScrollCallback(s.window, Mouse::mouseWheelCallback);
  glfwSetDropCallback(s.window, App::dropCallback);

  glEnable(GL_DEPTH_TEST);
  GLDebug::initialize();
  initImGui(s.window);
  return true;
}

void shutdownWindowAndImGui(AppState &s) {
  shutdownImGui();
  if (s.window) {
    glfwDestroyWindow(s.window);
    s.window = nullptr;
  }
  glfwTerminate();
}

bool initRuntimeSystems(AppState &s) {
  s.scene.setAssetManager(&s.assets);
  s.assets.setCookRoot(s.projectConfig.projectPath("Build/cooked"));

  const std::string mainVS =
      s.projectConfig.shaderPath(s.projectConfig.mainVertexShader);
  const std::string mainFS =
      s.projectConfig.shaderPath(s.projectConfig.mainFragmentShader);
  const std::string shadowVS =
      s.projectConfig.shaderPath(s.projectConfig.shadowVertexShader);
  const std::string shadowFS =
      s.projectConfig.shaderPath(s.projectConfig.shadowFragmentShader);
  const std::string hdrVS =
      s.projectConfig.shaderPath(s.projectConfig.hdrSkyVertexShader);
  const std::string hdrFS =
      s.projectConfig.shaderPath(s.projectConfig.hdrSkyFragmentShader);
  const std::string fireVS =
      s.projectConfig.shaderPath(s.projectConfig.fireBillboardVertexShader);
  const std::string fireFS =
      s.projectConfig.shaderPath(s.projectConfig.fireBillboardFragmentShader);
  const std::string smokeFS =
      s.projectConfig.shaderPath(s.projectConfig.smokeBillboardFragmentShader);
  const std::string projVS =
      s.projectConfig.shaderPath(s.projectConfig.projectileVertexShader);
  const std::string projFS =
      s.projectConfig.shaderPath(s.projectConfig.projectileFragmentShader);

  const std::string grassSide =
      s.projectConfig.assetPath(s.projectConfig.grassSideTexture);
  const std::string grassTop =
      s.projectConfig.assetPath(s.projectConfig.grassTopTexture);
  const std::string skyHdr = s.projectConfig.assetPath(s.projectConfig.skyHDR);
  const std::string fireTex =
      s.projectConfig.assetPath(s.projectConfig.fireTexture);

  if (!s.renderer.init(mainVS.c_str(), mainFS.c_str(), grassSide.c_str(),
                       grassTop.c_str(), grassTop.c_str(), shadowVS.c_str(),
                       shadowFS.c_str())) {
    return false;
  }

  s.sun.init();
  if (!s.sky.init(skyHdr, hdrVS, hdrFS))
    return false;

  if (!s.fire.init(fireTex.c_str(), fireVS.c_str(), fireFS.c_str(),
                   smokeFS.c_str())) {
    return false;
  }
  s.fire.setSize(1.0f);

  if (!s.projectiles.init(projVS.c_str(), projFS.c_str()))
    return false;

  (void)s.assets.registerShader(&s.renderer.shader(), mainVS, mainFS);
  (void)s.assets.registerShader(&s.renderer.shadowShader(), shadowVS, shadowFS);

  // ECS player
  s.playerId = s.scene.registry().create();
  auto &reg = s.scene.registry();
  reg.emplace<TransformComponent>(s.playerId);
  reg.get<TransformComponent>(s.playerId).position = glm::vec3(0.0f, 0.0f, 3.0f);
  reg.emplace<PhysicsComponent>(s.playerId);
  reg.emplace<CameraComponent>(s.playerId);
  reg.emplace<BoundsComponent>(s.playerId, BoundsComponent{1.0f});
  reg.emplace<LifecycleComponent>(s.playerId);
  reg.emplace<HierarchyComponent>(s.playerId);

  s.hasFire = false;
  s.lastT = (float)glfwGetTime();

  loadConfig(s, "editor_state.bin");
  return true;
}

void shutdownRuntimeSystems(AppState &s) {
  s.projectiles.shutdown();
  s.fire.shutdown();
  s.sky.shutdown();
  s.renderer.shutdown();
}

class WindowSubsystem final : public IEngineSubsystem {
public:
  explicit WindowSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "Window"; }
  bool initialize() override { return initWindowAndImGui(mState); }
  void shutdown() override { shutdownWindowAndImGui(mState); }

private:
  AppState &mState;
};

class RuntimeSystemsSubsystem final : public IEngineSubsystem {
public:
  explicit RuntimeSystemsSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "RuntimeSystems"; }
  std::vector<std::string> dependencies() const override { return {"Window"}; }
  bool initialize() override { return initRuntimeSystems(mState); }
  void shutdown() override { shutdownRuntimeSystems(mState); }

private:
  AppState &mState;
};

} // namespace

App::App() = default;
App::~App() = default;

void App::errorCallback(int, const char *description) {
  LOG_ERROR("Runtime", std::string("GLFW error: ") +
                           (description ? description : "<null>"));
}

void App::framebufferSizeCallback(GLFWwindow *, int width, int height) {
  glViewport(0, 0, width, height);
}

void App::dropCallback(GLFWwindow *window, int pathCount, const char **paths) {
  if (!window || !paths || pathCount <= 0)
    return;

  auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
  if (!state)
    return;

  for (int i = 0; i < pathCount; ++i) {
    if (paths[i] && paths[i][0] != '\0')
      state->pendingDropPaths.emplace_back(paths[i]);
  }
}

int App::run() {
  m = std::make_unique<AppState>();

#ifdef NDEBUG
  Logger::instance().setMinLevel(Logger::Level::Info);
#else
  Logger::instance().setMinLevel(Logger::Level::Trace);
#endif
  Logger::instance().setFileSink("Build/engine.log");
  CrashHandler::install("Build/crash_report.txt");
  LOG_INFO("Runtime", "Engine startup");

  (void)m->projectConfig.loadFromFile("project_config.json");

  m->events.subscribe<SaveConfigRequestedEvent>(
      [state = m.get()](const SaveConfigRequestedEvent &) {
        state->requestSaveConfig = true;
      });
  m->events.subscribe<LoadConfigRequestedEvent>(
      [state = m.get()](const LoadConfigRequestedEvent &) {
        state->requestLoadConfig = true;
      });
  m->events.subscribe<SaveProjectConfigRequestedEvent>(
      [state = m.get()](const SaveProjectConfigRequestedEvent &) {
        state->requestSaveProjectConfig = true;
      });
  m->events.subscribe<SpawnEntityRequestedEvent>(
      [state = m.get()](const SpawnEntityRequestedEvent &e) {
        state->pendingSpawnPaths.push_back(e.path);
      });
  m->events.subscribe<CreateEmptyEntityRequestedEvent>(
      [state = m.get()](const CreateEmptyEntityRequestedEvent &e) {
        state->pendingEmptyEntityNames.push_back(e.name);
      });
  m->events.subscribe<DeleteEntityRequestedEvent>(
      [state = m.get()](const DeleteEntityRequestedEvent &e) {
        state->pendingDeleteEntityIds.push_back(e.entityId);
      });
  m->events.subscribe<SaveSceneRequestedEvent>(
      [state = m.get()](const SaveSceneRequestedEvent &e) {
        state->pendingSceneSavePath = e.path;
      });
  m->events.subscribe<LoadSceneRequestedEvent>(
      [state = m.get()](const LoadSceneRequestedEvent &e) {
        state->pendingSceneLoadPath = e.path;
      });
  m->events.subscribe<UndoRequestedEvent>(
      [state = m.get()](const UndoRequestedEvent &) { state->requestUndo = true; });
  m->events.subscribe<RedoRequestedEvent>(
      [state = m.get()](const RedoRequestedEvent &) { state->requestRedo = true; });
  m->events.subscribe<SceneHistoryJumpRequestedEvent>(
      [state = m.get()](const SceneHistoryJumpRequestedEvent &e) {
        state->requestHistoryJump = e.index;
      });

  m->subsystems.registerSubsystem(std::make_unique<WindowSubsystem>(*m));
  m->subsystems.registerSubsystem(std::make_unique<RuntimeSystemsSubsystem>(*m));
  if (!m->subsystems.initializeAll())
    return -1;

  auto applyHistorySnapshot = [&](int idx) {
    if (idx < 0 || idx >= (int)m->historySnapshots.size())
      return;
    if (!m->scene.loadFromString(m->historySnapshots[idx]))
      return;
    m->historyCursor = idx;
    m->selectedEntityId = 0;
    m->selectedEntities.clear();
    m->lastClickedEntity = 0;
    m->playerId = 0;
    for (auto e : m->scene.registry().view<CameraComponent>()) {
      if (!m->scene.registry().has<LifecycleComponent>(e) ||
          m->scene.registry().get<LifecycleComponent>(e).state ==
              EntityLifecycleState::Alive) {
        m->playerId = e;
        break;
      }
    }
  };

  auto commitHistorySnapshot = [&](const std::string &label) {
    const std::string snap = m->scene.serializeToString();
    if (m->historyCursor >= 0 && m->historyCursor < (int)m->historySnapshots.size() &&
        m->historySnapshots[m->historyCursor] == snap)
      return;

    if (m->historyCursor + 1 < (int)m->historySnapshots.size()) {
      m->historySnapshots.erase(m->historySnapshots.begin() + m->historyCursor + 1,
                                m->historySnapshots.end());
      m->historyLabels.erase(m->historyLabels.begin() + m->historyCursor + 1,
                             m->historyLabels.end());
    }

    m->historySnapshots.push_back(snap);
    m->historyLabels.push_back(label);
    m->historyCursor = (int)m->historySnapshots.size() - 1;

    const int maxHistory = 128;
    if ((int)m->historySnapshots.size() > maxHistory) {
      const int trim = (int)m->historySnapshots.size() - maxHistory;
      m->historySnapshots.erase(m->historySnapshots.begin(),
                                m->historySnapshots.begin() + trim);
      m->historyLabels.erase(m->historyLabels.begin(),
                             m->historyLabels.begin() + trim);
      m->historyCursor -= trim;
      if (m->historyCursor < 0)
        m->historyCursor = 0;
    }
  };

  commitHistorySnapshot("Initial");

  // ---- Main loop ----
  while (!glfwWindowShouldClose(m->window)) {
    glfwPollEvents();

    float nowT = (float)glfwGetTime();
    float dt = nowT - m->lastT;
    m->lastT = nowT;
    if (dt > 0.05f)
      dt = 0.05f;

    // Toggle UI mode on ESC
    bool escDown = Keyboard::key(GLFW_KEY_ESCAPE);
    if (escDown && !m->escWasDown) {
      m->uiMode = !m->uiMode;
      glfwSetInputMode(m->window, GLFW_CURSOR,
                       m->uiMode ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
    }
    m->escWasDown = escDown;

    beginImGuiFrame();
    drawDockspace();

    // Track time for freeze support
    float renderTime = m->freezeTime ? m->frozenTime : nowT;
    if (!m->freezeTime)
      m->frozenTime = nowT;

    // Build EditorContext
    EditorContext ctx{
        m->uiMode,
        m->walkStep,
        m->runMult,
        m->jumpStrength,
        m->gravity,
        m->freezePhysics,
        m->mouseSensitivity,
        m->fov,
        m->sun,
        m->fire,
        m->cloud,
        m->sky,
        m->projectiles,
        m->scene,
        m->events,
        m->projectConfig,
        m->assets,
        m->terrainSize,
        m->terrainSpacing,
        m->solidSky,
        m->skyHorizon,
        m->skyTop,
        m->shadowStrength,
        m->shadowFarPlane,
        m->exposure,
        m->gamma,
        m->wireframe,
        m->disableShadows,
        m->disableClouds,
        m->freezeTime,
        dt,
        (int)m->scene.registry().view<TransformComponent>().size(),
        (int)(m->projectiles.count()),
        m->renderSystem.stats().drawn,
        m->renderSystem.stats().culled,
        m->frustumCulling,
        &m->lastRenderPassOrder,
        m->hotReloadEnabled,
        m->autoProcessImportQueue,
        &m->hotReloadMessages,
        &m->historyLabels,
        m->historyCursor};

    EditorUIOutput uiOut = m->editor.draw(ctx);
    bool sceneMutatedByCommands = false;
    if (m->requestSaveConfig) {
      saveConfig(*m, "editor_state.bin");
      m->requestSaveConfig = false;
    }
    if (m->requestLoadConfig) {
      loadConfig(*m, "editor_state.bin");
      m->requestLoadConfig = false;
    }
    if (m->requestSaveProjectConfig) {
      if (!m->projectConfig.saveToFile("project_config.json")) {
        LOG_ERROR("Runtime", "Failed to save project_config.json");
      }
      m->requestSaveProjectConfig = false;
    }
    if (!m->pendingSceneSavePath.empty()) {
      if (!m->scene.saveToFile(m->pendingSceneSavePath))
        LOG_ERROR("Runtime", "Failed to save scene: " + m->pendingSceneSavePath);
      m->pendingSceneSavePath.clear();
    }
    if (!m->pendingSceneLoadPath.empty()) {
      if (!m->scene.loadFromFile(m->pendingSceneLoadPath))
        LOG_ERROR("Runtime", "Failed to load scene: " + m->pendingSceneLoadPath);
      else {
        m->pendingHistoryCommit = true;
        m->pendingHistoryLabel = "Load Scene";
        sceneMutatedByCommands = true;
        m->playerId = 0;
        for (auto e : m->scene.registry().view<CameraComponent>()) {
          if (!m->scene.registry().has<LifecycleComponent>(e) ||
              m->scene.registry().get<LifecycleComponent>(e).state ==
                  EntityLifecycleState::Alive) {
            m->playerId = e;
            break;
          }
        }
      }
      m->pendingSceneLoadPath.clear();
    }

    if (m->requestHistoryJump >= 0) {
      applyHistorySnapshot(m->requestHistoryJump);
      m->requestHistoryJump = -1;
      sceneMutatedByCommands = true;
    } else if (m->requestUndo) {
      applyHistorySnapshot(m->historyCursor - 1);
      sceneMutatedByCommands = true;
    } else if (m->requestRedo) {
      applyHistorySnapshot(m->historyCursor + 1);
      sceneMutatedByCommands = true;
    }
    m->requestUndo = false;
    m->requestRedo = false;

    if (m->autoProcessImportQueue)
      m->assets.processImportQueue();
    if (m->hotReloadEnabled) {
      m->hotReloadMessages = m->assets.pollHotReload();
    } else {
      m->hotReloadMessages.clear();
    }

    for (const std::string &emptyName : m->pendingEmptyEntityNames) {
      (void)m->scene.createEmptyEntity(emptyName.empty() ? "Empty" : emptyName);
      m->pendingHistoryCommit = true;
      m->pendingHistoryLabel = "Create Entity";
      sceneMutatedByCommands = true;
    }
    m->pendingEmptyEntityNames.clear();

    for (uint32_t entityId : m->pendingDeleteEntityIds) {
      if (entityId != 0) {
        m->scene.deleteEntity(entityId);
        m->pendingHistoryCommit = true;
        m->pendingHistoryLabel = "Delete Entity";
        sceneMutatedByCommands = true;
      }
    }
    m->pendingDeleteEntityIds.clear();

    for (const std::string &path : m->pendingSpawnPaths) {
      const uint32_t spawnedId = m->scene.spawnFromFile(path);
      if (spawnedId == 0) {
        LOG_ERROR("Runtime", "Failed to load model from path: " + path);
      } else {
        m->pendingHistoryCommit = true;
        m->pendingHistoryLabel = "Spawn Asset";
        sceneMutatedByCommands = true;
      }
    }
    m->pendingSpawnPaths.clear();

    if (!m->pendingDropPaths.empty()) {
      for (const std::string &path : m->pendingDropPaths) {
        const uint32_t spawnedId = m->scene.spawnFromFile(path);
        if (spawnedId == 0)
          LOG_ERROR("Runtime", "Failed to load dropped model: " + path);
        else {
          m->pendingHistoryCommit = true;
          m->pendingHistoryLabel = "Spawn Asset";
          sceneMutatedByCommands = true;
        }
      }
      m->pendingDropPaths.clear();
    }

    const size_t entityCountBeforeFlush =
        m->scene.registry().view<TransformComponent>().size();
    m->scene.flushPendingDestroy();
    const size_t entityCountAfterFlush =
        m->scene.registry().view<TransformComponent>().size();
    if (entityCountAfterFlush != entityCountBeforeFlush) {
      m->pendingHistoryCommit = true;
      m->pendingHistoryLabel = "Destroy Entity";
      sceneMutatedByCommands = true;
    }

    // Wireframe toggle
    if (m->wireframe)
      GLStateCache::instance().setPolygonMode(GL_LINE);
    else
      GLStateCache::instance().setPolygonMode(GL_FILL);

    bool allowGameInput = (!m->uiMode && !uiOut.wantCaptureKeyboard);

    // --- ECS Systems Update ---
    if (allowGameInput) {
      m->movementSystem.update(m->scene.registry(), dt, m->walkStep, m->runMult,
                               m->jumpStrength);
    }

    // Camera System (Mouse Look)
    bool allowMouseLook =
        (!m->uiMode && !uiOut.wantCaptureMouse && !ImGuizmo::IsUsing());
    if (allowMouseLook) {
      m->cameraSystem.update(m->scene.registry(), m->mouseSensitivity);
    } else {
      Mouse::getDX(); // Consume delta
      Mouse::getDY();
    }

    // (Turret logic removed — legacy tank entity no longer exists)

    // Recover Player Position for View Matrix
    glm::vec3 cameraPos(0, 0, 3);
    glm::vec3 cameraFront(0, 0, -1);
    glm::vec3 cameraUp(0, 1, 0);

    if (m->playerId != 0) {
      if (m->scene.registry().has<TransformComponent>(m->playerId)) {
        cameraPos =
            m->scene.registry().get<TransformComponent>(m->playerId).position;
      }
      if (m->scene.registry().has<CameraComponent>(m->playerId)) {
        const auto &cam = m->scene.registry().get<CameraComponent>(m->playerId);
        cameraFront = cam.front;
        cameraUp = cam.up;
      }
    }

    glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);

    int fbW, fbH;
    glfwGetFramebufferSize(m->window, &fbW, &fbH);
    glm::mat4 projection = glm::perspective(
        glm::radians(m->fov), (float)fbW / (float)fbH, 0.1f, 500.0f);

    // (Turret projectile spawning removed — legacy tank entity no longer
    // exists)

    // Draw Gizmo / Outliner via EditorUI class
    {
      EditorSelectionState selState{m->selectedEntityId,    m->selectedEntities,
                                    m->lastClickedEntity,   m->editObjPart,
                                    m->selectedObjPartName, (int &)m->gizmoOp,
                                    (int &)m->gizmoMode,    m->renaming,
                                    m->renameBuf,           m->outlinerFilter,
                                    m->focusDistance};

      if (m->editor.drawGizmo(m->uiMode, view, projection, m->scene, m->sun,
                              m->events, selState, cameraPos)) {
        m->pendingHistoryCommit = true;
        m->pendingHistoryLabel = "Edit Scene";
      }
    }

    // Component Inspector
    if (m->editor.drawInspector(m->uiMode, m->scene.registry(),
                                m->selectedEntityId)) {
      m->pendingHistoryCommit = true;
      m->pendingHistoryLabel = "Edit Scene";
    }

    if (m->pendingHistoryCommit) {
      const bool interacting =
          ImGuizmo::IsUsing() || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(0);
      if (!interacting || sceneMutatedByCommands) {
        commitHistorySnapshot(m->pendingHistoryLabel.empty() ? "Edit Scene"
                                                             : m->pendingHistoryLabel);
        m->pendingHistoryCommit = false;
        m->pendingHistoryLabel.clear();
      }
    }

    bool editingSun =
        (m->uiMode && m->selectedEntityId == 0 && ImGuizmo::IsUsing());
    if (!editingSun)
      m->sun.update(dt, renderTime);

    m->projectiles.update(dt);
    m->renderSystem.setViewProjection(projection * view);
    m->renderSystem.setCameraPosition(cameraPos);
    m->renderSystem.setCullingEnabled(m->frustumCulling);

    m->renderGraph.clear();
    if (!m->disableShadows) {
      m->renderGraph.addPass({"ShadowPass",
                              {},
                              [&]() {
                                renderShadowPass(*m, m->sun.sunPos, 1.0f,
                                                 m->shadowFarPlane);
                              }});
    }
    m->renderGraph.addPass(
        {"MainPass",
         m->disableShadows ? std::vector<std::string>{}
                           : std::vector<std::string>{"ShadowPass"},
         [&]() {
           renderMainPass(*m, view, projection, cameraPos, cameraFront,
                          cameraUp, renderTime);
         }});
    (void)m->renderGraph.execute();
    m->lastRenderPassOrder = m->renderGraph.lastExecutionOrder();

    endImGuiFrame();
    glfwSwapBuffers(m->window);
  }

  m->subsystems.shutdownAll();
  LOG_INFO("Runtime", "Engine shutdown");
  Logger::instance().clearFileSink();
  return 0;
}

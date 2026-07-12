#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "App.h"
#include "AssetManager.h"
#include "CloudFX.h"
#include "CrashHandler.h"
#include "ECS/Components.h"
#include "ECS/Systems/CameraSystem.h"
#include "ECS/Systems/RenderSystem.h"
#include "EditorTheme.h"
#include "EditorUI.h"
#include "EngineEvents.h"
#include "EventBus.h"
#include "FBXModel.h"
#include "FireFX.h"
#include "GLDebug.h"
#include "GLModelBackend.h"
#include "GLStateCache.h"
#include "HDRSky.h"
#include "IEngineSubsystem.h"
#include "Keyboard.h"
#include "Logger.h"
#include "Mouse.h"
#include "MousePicking.h"
#include "OBJModel.h"
#include "ProjectConfig.h"
#include "ProjectileSystem.h"
#include "RenderGraph.h"
#include "Renderer.h"
#include "Scene.h"
#include "Shader.h"
#include "SubsystemManager.h"
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
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>

#include "AppState.h"
#include "AudioSubsystem.h"
#include "CoreAppLayer.h"
#include "EditorSubsystem.h"
#include "NetworkRuntimeSubsystem.h"
#include "PhysicsRuntimeSubsystem.h"
#include "RenderLoopSubsystem.h"
#include "ScriptRuntimeSubsystem.h"
#include "TerrainRuntimeSubsystem.h"
#include "imgui.h"

namespace {

void startupTrace(const char *message) {
  if (!message)
    return;
  std::fprintf(stderr, "[glGen startup] %s\n", message);
  std::fflush(stderr);
}

void startupGlTrace(const char *stage) {
  GLenum err = glGetError();
  if (err == GL_NO_ERROR)
    return;

  const std::string label = stage ? stage : "startup";
  while (err != GL_NO_ERROR) {
    const std::string msg =
        label + " generated GL error " + std::to_string((int)err);
    startupTrace(msg.c_str());
    LOG_ERROR("Render", msg);
    err = glGetError();
  }
}

#include <fstream>

void saveConfig(const AppState &s, const char *filename) {
  // TODO: Port to ECS
}

void loadConfig(AppState &s, const char *filename) {
  // TODO: Port to ECS
}

// drawSkyUI removed — merged into EditorUI::draw() via EditorContext

// Render passes have been extracted to RenderLoopSubsystem

bool initWindowAndImGui(AppState &s) {
  startupTrace("initWindowAndImGui: begin");
  glfwSetErrorCallback(App::errorCallback);
  if (!glfwInit()) {
    const char *description = nullptr;
    glfwGetError(&description);
    std::fprintf(stderr, "[glGen startup] glfwInit failed: %s\n",
                 description ? description : "<no description>");
    std::fflush(stderr);
    return false;
  }

  auto applyWindowHints = [](int minorVersion) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minorVersion);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_STENCIL_BITS, 8);
  };
  applyWindowHints(3);

  // Make the engine windowed instead of exclusive fullscreen
  s.scrW = 1600;
  s.scrH = 900;

  s.window = glfwCreateWindow(s.scrW, s.scrH, "glGen Engine", nullptr, nullptr);
  if (!s.window) {
    const char *description = nullptr;
    glfwGetError(&description);
    std::fprintf(stderr,
                 "[glGen startup] OpenGL 4.3 window failed, retrying 4.1: %s\n",
                 description ? description : "<no description>");
    std::fflush(stderr);
    applyWindowHints(1);
    s.window =
        glfwCreateWindow(s.scrW, s.scrH, "glGen Engine", nullptr, nullptr);
  }
  if (!s.window) {
    const char *description = nullptr;
    glfwGetError(&description);
    std::fprintf(stderr, "[glGen startup] glfwCreateWindow failed: %s\n",
                 description ? description : "<no description>");
    std::fflush(stderr);
    return false;
  }

  glfwMakeContextCurrent(s.window);
  glfwSetInputMode(s.window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  glfwSetWindowUserPointer(s.window, &s);

  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    startupTrace("initWindowAndImGui: gladLoadGLLoader failed");
    LOG_FATAL("Runtime", "Failed to initialize OpenGL libraries");
    return false;
  }

  int fbW, fbH;
  glfwGetFramebufferSize(s.window, &fbW, &fbH);
  s.scrW = fbW;
  s.scrH = fbH;

  glViewport(0, 0, s.scrW, s.scrH);
  glfwSetFramebufferSizeCallback(s.window, App::framebufferSizeCallback);
  glfwSetKeyCallback(s.window, Keyboard::keyCallBack);
  glfwSetCursorPosCallback(s.window, Mouse::cursorPosCallback);
  glfwSetMouseButtonCallback(s.window, Mouse::mouseButtonCallback);
  glfwSetScrollCallback(s.window, Mouse::mouseWheelCallback);
  glfwSetDropCallback(s.window, App::dropCallback);

  glEnable(GL_DEPTH_TEST);
  GLDebug::initialize();
  startupTrace("initWindowAndImGui: success");
  return true;
}

void shutdownWindowAndImGui(AppState &s) {
  if (s.window) {
    glfwDestroyWindow(s.window);
    s.window = nullptr;
  }
  glfwTerminate();
}

bool initRuntimeSystems(AppState &s) {
  s.scene.setAssetManager(&s.assets);
  InstallGLModelBackend(s.assets);
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
  const std::string blackHoleFS = s.projectConfig.shaderPath("black_hole.frag");
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
  const std::string laserVS = s.projectConfig.shaderPath("laser.vert");
  const std::string laserFS = s.projectConfig.shaderPath("laser.frag");
  const std::string cloudVS = s.projectConfig.shaderPath("cloud.vert");
  const std::string cloudFS = s.projectConfig.shaderPath("cloud.frag");

  const std::string grassSide =
      s.projectConfig.assetPath(s.projectConfig.grassSideTexture);
  const std::string grassTop =
      s.projectConfig.assetPath(s.projectConfig.grassTopTexture);
  const std::string skyHdr = s.projectConfig.assetPath(s.projectConfig.skyHDR);
  const std::string fireTex =
      s.projectConfig.assetPath(s.projectConfig.fireTexture);

  const std::string outlineVS = s.projectConfig.shaderPath("outline.vert");
  const std::string outlineFS = s.projectConfig.shaderPath("outline.frag");

  const std::string ppQuadVS =
      s.projectConfig.shaderPath(s.projectConfig.screenQuadVertexShader);
  const std::string ppBloomExtractFS =
      s.projectConfig.shaderPath(s.projectConfig.bloomExtractFragmentShader);
  const std::string ppBloomBlurFS =
      s.projectConfig.shaderPath(s.projectConfig.bloomBlurFragmentShader);
  const std::string ppSSAOFS =
      s.projectConfig.shaderPath(s.projectConfig.ssaoFragmentShader);
  const std::string ppSSAOBlurFS =
      s.projectConfig.shaderPath(s.projectConfig.ssaoBlurFragmentShader);
  const std::string ppVolumetricFS =
      s.projectConfig.shaderPath(s.projectConfig.volumetricFogFragmentShader);
  const std::string ppBloomCompositeFS =
      s.projectConfig.shaderPath(s.projectConfig.bloomCompositeFragmentShader);
  const std::string terrainFlatVS = s.projectConfig.shaderPath("terrain_flat.vert");
  const std::string terrainFlatFS = s.projectConfig.shaderPath("terrain_flat.frag");

  if (!s.renderer.init(mainVS.c_str(), mainFS.c_str(), grassSide.c_str(),
                       grassTop.c_str(), grassTop.c_str(), shadowVS.c_str(),
                       shadowFS.c_str())) {
    return false;
  }
  startupGlTrace("runtime init: renderer.init");

  if (s.skyUI.skyHDRPath.empty())
    s.skyUI.skyHDRPath = skyHdr;

  if (!s.sky.init(s.skyUI.skyHDRPath, hdrVS, hdrFS))
    return false;
  startupGlTrace("runtime init: sky.init");

  if (!s.blackHole.init(hdrVS, blackHoleFS))
    LOG_WARN("Runtime", "Failed to initialize BlackHoleRenderer; normal sky fallback will be used");
  startupGlTrace("runtime init: blackHole.init");

  if (!s.fire.init(fireTex.c_str(), fireVS.c_str(), fireFS.c_str(),
                   smokeFS.c_str())) {
    return false;
  }
  s.fire.setSize(1.0f);

  s.postProcessor.init(ppQuadVS, ppBloomExtractFS, ppBloomBlurFS, ppSSAOFS,
                       ppSSAOBlurFS, ppVolumetricFS, ppBloomCompositeFS, s.scrW,
                       s.scrH);
  startupGlTrace("runtime init: postProcessor.init");

  if (!s.projectiles.init(projVS.c_str(), projFS.c_str(), laserVS.c_str(),
                          laserFS.c_str()))
    return false;
  startupGlTrace("runtime init: projectiles.init");

  if (!s.cloud.init(cloudVS, cloudFS))
    LOG_WARN("Runtime", "Failed to initialize CloudFX");
  startupGlTrace("runtime init: cloud.init");

  const std::string fireflyVS = s.projectConfig.shaderPath("firefly.vert");
  const std::string fireflyFS = s.projectConfig.shaderPath("firefly.frag");
  if (!s.fireflies.init(fireflyVS.c_str(), fireflyFS.c_str()))
    LOG_WARN("Runtime", "Failed to initialize FireflySystem");
  startupGlTrace("runtime init: fireflies.init");

  (void)s.assets.registerShader(&s.renderer.shader(), mainVS, mainFS);
  (void)s.assets.registerShader(&s.renderer.shadowShader(), shadowVS, shadowFS);
  if (s.cloud.shader())
    (void)s.assets.registerShader(s.cloud.shader(), cloudVS, cloudFS);
  s.terrainFlatShader =
      std::make_unique<Shader>(terrainFlatVS.c_str(), terrainFlatFS.c_str());
  (void)s.assets.registerShader(s.terrainFlatShader.get(), terrainFlatVS,
                                terrainFlatFS);

  // Initialize and register outline shader
  s.outlineShader =
      std::make_unique<Shader>(outlineVS.c_str(), outlineFS.c_str());
  (void)s.assets.registerShader(s.outlineShader.get(), outlineVS, outlineFS);

  // ECS player
  s.gameplay.playerId = s.scene.registry().create();
  auto &reg = s.scene.registry();
  reg.emplace<TransformComponent>(s.gameplay.playerId);
  reg.get<TransformComponent>(s.gameplay.playerId).position =
      glm::vec3(0.0f, 0.0f, 3.0f);
  reg.emplace<RigidbodyComponent>(s.gameplay.playerId).type =
      RigidbodyComponent::Type::Kinematic;
  reg.emplace<ColliderComponent>(s.gameplay.playerId);
  reg.emplace<CameraComponent>(s.gameplay.playerId);
  reg.emplace<NameComponent>(s.gameplay.playerId, "Player");
  reg.emplace<ScriptComponent>(s.gameplay.playerId).scriptPath =
      "scripts/fps_controller.lua";
  reg.emplace<BoundsComponent>(s.gameplay.playerId, BoundsComponent{1.0f});
  reg.emplace<LifecycleComponent>(s.gameplay.playerId);
  reg.emplace<HierarchyComponent>(s.gameplay.playerId);

  s.hasFire = false;
  s.lastT = (float)glfwGetTime();

  loadConfig(s, "editor_state.bin");
  return true;
}

void shutdownRuntimeSystems(AppState &s) {
  s.postProcessor.shutdown();
  s.projectiles.shutdown();
  s.fireflies.shutdown();
  s.cloud.shutdown();
  s.fire.shutdown();
  s.blackHole.shutdown();
  s.sky.shutdown();
  s.renderer.shutdown();
  s.terrainFlatShader.reset();
}

class WindowSubsystem final : public IEngineSubsystem {
public:
  explicit WindowSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "Window"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Platform; }
  bool initialize() override { return initWindowAndImGui(mState); }
  void shutdown() override { shutdownWindowAndImGui(mState); }

private:
  AppState &mState;
};

class RuntimeSystemsSubsystem final : public IEngineSubsystem {
public:
  explicit RuntimeSystemsSubsystem(AppState &state) : mState(state) {}

  std::string name() const override { return "RuntimeSystems"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Foundation; }
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

void App::framebufferSizeCallback(GLFWwindow *window, int width, int height) {
  glViewport(0, 0, width, height);
  if (auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window))) {
    state->scrW = width;
    state->scrH = height;
  }
}

void App::dropCallback(GLFWwindow *window, int pathCount, const char **paths) {
  if (!window || !paths || pathCount <= 0)
    return;

  auto *state = static_cast<AppState *>(glfwGetWindowUserPointer(window));
  if (!state)
    return;

  for (int i = 0; i < pathCount; ++i) {
    if (paths[i] && paths[i][0] != '\0')
      state->editorSubsystem->pending().pendingDropPaths.emplace_back(paths[i]);
  }
}

int App::run() {
  startupTrace("App::run begin");
  m = std::make_unique<AppState>();

#ifdef NDEBUG
  Logger::instance().setMinLevel(Logger::Level::Info);
#else
  Logger::instance().setMinLevel(Logger::Level::Trace);
#endif

  std::filesystem::create_directories("Build");
  Logger::instance().setFileSink("Build/engine.log");
  startupTrace("Logger initialized");
  CrashHandler::install("Build/crash_report.txt");
  LOG_INFO("Runtime", "Engine startup");

  startupTrace("Loading project_config.json");
  const bool configLoaded = m->projectConfig.loadFromFile("project_config.json");
  std::fprintf(stderr, "[glGen startup] project_config.json loaded: %s\n",
               configLoaded ? "true" : "false");
  std::fflush(stderr);

  m->subsystems.registerSubsystem(std::make_unique<WindowSubsystem>(*m));

  auto audioSubsys = std::make_unique<AudioSubsystem>(*m);
  m->audioSubsystem = audioSubsys.get();
  m->subsystems.registerSubsystem(std::move(audioSubsys));

  m->subsystems.registerSubsystem(
      std::make_unique<RuntimeSystemsSubsystem>(*m));
  m->subsystems.registerSubsystem(
      std::make_unique<PhysicsRuntimeSubsystem>(*m));
  m->subsystems.registerSubsystem(
      std::make_unique<ScriptRuntimeSubsystem>(*m));
  m->subsystems.registerSubsystem(
      std::make_unique<NetworkRuntimeSubsystem>(*m));
  m->subsystems.registerSubsystem(
      std::make_unique<TerrainRuntimeSubsystem>(*m));

  auto editorSubsys = std::make_unique<EditorSubsystem>(*m);
  m->editorSubsystem = editorSubsys.get();
  m->subsystems.registerSubsystem(std::move(editorSubsys));

  auto renderLoopSubsys = std::make_unique<RenderLoopSubsystem>(*m);
  m->renderLoopSubsystem = renderLoopSubsys.get();
  m->subsystems.registerSubsystem(std::move(renderLoopSubsys));

  auto coreAppLayer = std::make_unique<CoreAppLayer>(*m);
  m->coreAppLayer = coreAppLayer.get();
  m->subsystems.registerSubsystem(std::move(coreAppLayer));
  m->subsystems.registerProfile({"Editor", {"CoreAppLayer"}});
  m->subsystems.registerProfile({"Runtime", {"RenderLoopSubsystem"}});

  startupTrace("Initializing startup profile: Editor");
  if (!m->subsystems.initializeProfile("Editor")) {
    startupTrace("Startup profile initialization failed");
    return -1;
  }
  startupTrace("Startup profile initialized");

  // ---- Main loop ----
  startupTrace("Entering main loop");
  while (!glfwWindowShouldClose(m->window)) {
    glfwPollEvents();

    float nowT = (float)glfwGetTime();
    float dt = nowT - m->lastT;
    m->lastT = nowT;
    if (dt > 0.05f)
      dt = 0.05f;

    if (m->coreAppLayer)
      m->coreAppLayer->update(dt, nowT);

    glfwSwapBuffers(m->window);
  }

  m->subsystems.shutdownAll();
  LOG_INFO("Runtime", "Engine shutdown");
  Logger::instance().clearFileSink();
  startupTrace("App::run end");
  return 0;
}

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "App.h"
#include "ECS/Components.h"
#include "ECS/Systems/CameraSystem.h"
#include "ECS/Systems/MovementSystem.h"
#include "ECS/Systems/RenderSystem.h"
#include "EditorUI.h"
#include "FBXModel.h"
#include "FireFX.h"
#include "HDRSky.h"
#include "Keyboard.h"
#include "Mouse.h"
#include "OBJModel.h"
#include "ProjectileSystem.h"
#include "Renderer.h"
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
#include <iostream>
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
  HDRSky sky;
  FireFX fire;
  EditorUI editor;
  ProjectileSystem projectiles;

  // ECS Systems
  RenderSystem renderSystem;
  MovementSystem movementSystem;
  CameraSystem cameraSystem;

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
};

namespace {

void initImGui(GLFWwindow *window) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
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

void drawSkyUI(AppState &s) {
  if (!s.uiMode)
    return;

  ImGui::Begin("Sky");
  ImGui::Checkbox("Solid/Gradient sky (no HDR)", &s.solidSky);
  ImGui::ColorEdit3("Horizon", s.skyHorizon);
  ImGui::ColorEdit3("Top", s.skyTop);
  ImGui::End();

  s.sky.setSolidSky(s.solidSky);
  s.sky.setSkyColors(
      glm::vec3(s.skyHorizon[0], s.skyHorizon[1], s.skyHorizon[2]),
      glm::vec3(s.skyTop[0], s.skyTop[1], s.skyTop[2]));
}

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
  s.sky.draw(view, projection, 1.0f, 2.2f);

  // 1. CLEANUP: Unbind the Skybox's HDR texture from Unit 0
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);

  // 2. ACTIVATE SHADER BEFORE SETTING UNIFORMS
  s.renderer.shader().activate();

  // 3. CRITICAL FIX: Explicitly set Sampler Indices
  // Tell the shader: "texture1 is on Unit 0, shadowCube is on Unit 1"
  s.renderer.shader().setInt("texture1", 0);
  s.renderer.shader().setInt("shadowCube", 1);

  s.renderer.setFrameUniforms(view, projection, s.mixVal, nowT, s.sun.sunColor,
                              s.sun.ambientStrength, cameraPos,
                              s.sun.glowStrength, lightPos, far_plane, 1.5f);

  // ... (fire code) ...
  s.renderer.shader().setBool("uHasFire", false);

  // ECS Rendering
  s.renderSystem.update(s.scene.registry(), s.renderer.shader(), false);

  s.projectiles.draw(view, projection, 0.25f);

  s.renderer.shader().activate();
  s.sun.draw(s.renderer.shader(), cameraFront, cameraUp);
}

} // namespace

App::App() = default;
App::~App() = default;

void App::errorCallback(int, const char *description) {
  std::cerr << "Error: " << description << "\n";
}

void App::framebufferSizeCallback(GLFWwindow *, int width, int height) {
  glViewport(0, 0, width, height);
}

int App::run() {
  m = std::make_unique<AppState>();

  glfwSetErrorCallback(App::errorCallback);
  if (!glfwInit())
    return -1;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

  // Borderless windowed fullscreen
  GLFWmonitor *mon = glfwGetPrimaryMonitor();
  const GLFWvidmode *mode = glfwGetVideoMode(mon);

  m->scrW = mode->width;
  m->scrH = mode->height;

  m->window = glfwCreateWindow(m->scrW, m->scrH, "glGen", nullptr, nullptr);
  if (!m->window) {
    glfwTerminate();
    return -1;
  }

  // Move it onto the primary monitor (multi-monitor safe)
  int mx, my;
  glfwGetMonitorPos(mon, &mx, &my);
  glfwSetWindowPos(m->window, mx, my);

  glfwMakeContextCurrent(m->window);
  glfwSetInputMode(m->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

  if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
    std::cout << "Failed to initialize OpenGL libraries\n";
    glfwDestroyWindow(m->window);
    glfwTerminate();
    return -1;
  }

  glViewport(0, 0, m->scrW, m->scrH);
  glfwSetFramebufferSizeCallback(m->window, App::framebufferSizeCallback);

  glfwSetKeyCallback(m->window, Keyboard::keyCallBack);
  glfwSetCursorPosCallback(m->window, Mouse::cursorPosCallback);
  glfwSetMouseButtonCallback(m->window, Mouse::mouseButtonCallback);
  glfwSetScrollCallback(m->window, Mouse::mouseWheelCallback);

  glEnable(GL_DEPTH_TEST);

  initImGui(m->window);

  // ---- Init systems ----
  m->renderer.init(
      "/Users/edjan03/Downloads/glGen-main/shaders/glsl/vertex_core.glsl",
      "/Users/edjan03/Downloads/glGen-main/shaders/glsl/fragment_core.glsl",
      "/Users/edjan03/Downloads/glGen-main/assets/grass_side.png",
      "/Users/edjan03/Downloads/glGen-main/assets/grass_top.png",
      "/Users/edjan03/Downloads/glGen-main/assets/grass_top.png");

  m->sun.init();
  m->sky.init(
      "/Users/edjan03/Downloads/glGen-main/assets/hdr/hdr_1/cloudy.hdr");

  m->fire.init("/Users/edjan03/Downloads/glGen-main/assets/"
               "pngtree-realistic-3d-fire-flame-effect-for-designs-png-image_"
               "13631567.png");
  m->fire.setSize(1.0f);

  m->projectiles.init(
      "/Users/edjan03/Downloads/glGen-main/shaders/glsl/projectile.vert",
      "/Users/edjan03/Downloads/glGen-main/shaders/glsl/projectile.frag");

  // ---- Scene setup (ECS Migration) ----

  // 1. Town (New Map)
  uint32_t townId = m->scene.registry().create();
  {
    auto &reg = m->scene.registry();
    reg.emplace<TransformComponent>(townId);
    reg.emplace<NameComponent>(townId, "Town");
    auto &tr = reg.get<TransformComponent>(townId);
    tr.position = glm::vec3(0.0f, 0.0f, 0.0f);
    tr.scale = glm::vec3(1.0f);

    OBJModel *model = m->scene.getOrLoadOBJ(
        "/Users/edjan03/Downloads/glGen-main/assets/mctown/Mineways2Skfb.obj");
    reg.emplace<MeshComponent>(townId, model);
  }

  // --- Initialize ECS Player ---
  {
    m->playerId = m->scene.registry().create();
    auto &reg = m->scene.registry();

    // Transform
    reg.emplace<TransformComponent>(m->playerId);
    auto &tr = reg.get<TransformComponent>(m->playerId);
    tr.position = glm::vec3(0.0f, 0.0f, 3.0f);

    // Physics
    reg.emplace<PhysicsComponent>(m->playerId);

    // Camera
    reg.emplace<CameraComponent>(m->playerId);
  }

  m->hasFire = false; // No fire anchor in current scene
  m->lastT = (float)glfwGetTime();

  // NEW: Auto-load
  loadConfig(*m, "editor_state.bin");

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

    // EditorUI draw (treePos/treeScale are legacy placeholders)
    glm::vec3 dummyPos(0.0f), dummyScale(1.0f);
    EditorUIOutput uiOut = m->editor.draw(
        m->uiMode, m->walkStep, m->runMult, m->jumpStrength, m->gravity,
        m->freezePhysics, m->mouseSensitivity, m->fov, m->sun, m->fire,
        m->terrainSize, m->terrainSpacing, dummyPos, dummyScale);
    if (uiOut.saveRequested) {
      saveConfig(*m, "editor_state.bin");
    }
    if (uiOut.loadRequested) {
      loadConfig(*m, "editor_state.bin");
    }
    m->sky.setRotationDegrees(uiOut.skyRotDeg);

    drawSkyUI(*m);

    bool allowGameInput = (!m->uiMode && !uiOut.wantCaptureKeyboard);

    // --- ECS Systems Update ---
    if (allowGameInput) {
      m->movementSystem.update(m->scene.registry(), dt);
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

      m->editor.drawGizmo(m->uiMode, view, projection, m->scene, m->sun,
                          selState, cameraPos);
    }

    bool editingSun =
        (m->uiMode && m->selectedEntityId == 0 && ImGuizmo::IsUsing());
    if (!editingSun)
      m->sun.update(dt, nowT);

    m->projectiles.update(dt);

    renderShadowPass(*m, m->sun.sunPos, 1.0f, 250.0f);
    renderMainPass(*m, view, projection, cameraPos, cameraFront, cameraUp,
                   nowT);

    endImGuiFrame();
    glfwSwapBuffers(m->window);
  }

  // Shutdown
  m->sky.shutdown();
  shutdownImGui();

  glfwDestroyWindow(m->window);
  glfwTerminate();
  return 0;
}

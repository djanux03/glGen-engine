#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include "App.h"
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
  FBXModel myModel;
  // Scene ids
  Scene::EntityId treeId = 0;
  Scene::EntityId carId = 0;
  Scene::EntityId starwarsId = 0;

  // Terrain/editor vars
  int terrainSize = 10;
  float terrainSpacing = 1.0f;

  // Fire anchor
  glm::vec3 fireLocal = glm::vec3(0.0f);
  bool hasFireAnchor = false;
  bool hasFire = false;

  // Fire turret
  glm::vec3 turretLocal = glm::vec3(0.0f);
  bool hasTurretAnchor = false;
  float turretYawDeg = 0.0f;
  glm::vec3 turretPivotLocal = glm::vec3(0);
  bool hasTurretPivot = false;

  // Sub-object selection
  std::string selectedObjPartName;
  bool editObjPart = false;

  // Camera/player
  float x = 0.0f, y = 0.0f, z = 3.0f;
  float yaw = -90.0f;
  float pitch = 0.0f;

  glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
  glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);

  // Jump/physics
  float groundY = 0.0f;
  float yVel = 0.0f;
  bool onGround = true;
  bool spaceWasDown = false;

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
  std::vector<int> selectedEntities;
  int lastClickedEntity = -1;
  bool renaming = false;
  char renameBuf[128] = "";
  float focusDistance = 12.0f;
  int selectedEntityIndex = 0;

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
  std::ofstream out(filename, std::ios::binary);
  if (!out.is_open()) {
    std::cerr << "Failed to save config to " << filename << "\n";
    return;
  }

  // --- 1. Save Globals (Sun, Fire, Player, etc.) ---
  AppConfig cfg;
  cfg.sunPos = s.sun.sunPos;
  cfg.sunDir = s.sun.sunDir;
  cfg.sunColor = s.sun.sunColor;
  cfg.sunSize = s.sun.sunSize;
  cfg.ambientStrength = s.sun.ambientStrength;

  cfg.fireEnabled = s.fire.params().enabled;
  cfg.fireOffset = s.fire.params().offset;
  cfg.fireSize = s.fire.params().size;
  cfg.fireIntensity = s.fire.params().intensity;

  cfg.x = s.x;
  cfg.y = s.y;
  cfg.z = s.z;
  cfg.yaw = s.yaw;
  cfg.pitch = s.pitch;

  cfg.terrainSize = s.terrainSize;
  cfg.terrainSpacing = s.terrainSpacing;
  cfg.turretYaw = s.turretYawDeg;

  // Write global config header
  out.write(reinterpret_cast<const char *>(&cfg), sizeof(AppConfig));

  // --- 2. Save Entities ---
  const auto &entities = s.scene.entities();
  size_t count = entities.size();

  // Write count first
  out.write(reinterpret_cast<const char *>(&count), sizeof(count));

  for (const auto &e : entities) {
    EntitySaveData eData;

    // Safely copy name
    std::strncpy(eData.name, e.name.c_str(), sizeof(eData.name) - 1);
    eData.name[sizeof(eData.name) - 1] = '\0';

    eData.pos = e.tr.pos;
    eData.rot = e.tr.rotDeg;
    eData.scale = e.tr.scale;

    out.write(reinterpret_cast<const char *>(&eData), sizeof(EntitySaveData));
  }

  out.close();
  std::cout << "Saved " << count << " entities to " << filename << "\n";
}

void loadConfig(AppState &s, const char *filename) {
  std::ifstream in(filename, std::ios::binary);
  if (!in.is_open()) {
    std::cerr << "No config file found at " << filename << "\n";
    return;
  }

  // --- 1. Load Globals ---
  AppConfig cfg;
  in.read(reinterpret_cast<char *>(&cfg), sizeof(AppConfig));

  s.sun.sunPos = cfg.sunPos;
  s.sun.sunDir = cfg.sunDir;
  s.sun.sunColor = cfg.sunColor;
  s.sun.sunSize = cfg.sunSize;
  s.sun.ambientStrength = cfg.ambientStrength;

  s.fire.params().enabled = cfg.fireEnabled;
  s.fire.params().offset = cfg.fireOffset;
  s.fire.params().size = cfg.fireSize;
  s.fire.params().intensity = cfg.fireIntensity;

  s.x = cfg.x;
  s.y = cfg.y;
  s.z = cfg.z;
  s.yaw = cfg.yaw;
  s.pitch = cfg.pitch;

  // Recalc camera front
  glm::vec3 front;
  front.x = cos(glm::radians(s.yaw)) * cos(glm::radians(s.pitch));
  front.y = sin(glm::radians(s.pitch));
  front.z = sin(glm::radians(s.yaw)) * cos(glm::radians(s.pitch));
  s.cameraFront = glm::normalize(front);

  s.terrainSize = cfg.terrainSize;
  s.terrainSpacing = cfg.terrainSpacing;
  s.turretYawDeg = cfg.turretYaw;

  // --- 2. Load Entities ---
  size_t count = 0;
  if (in.read(reinterpret_cast<char *>(&count), sizeof(count))) {
    for (size_t i = 0; i < count; ++i) {
      EntitySaveData eData;
      in.read(reinterpret_cast<char *>(&eData), sizeof(EntitySaveData));

      // Find entity by Name in the existing scene
      // (Assuming entities are already created by App setup)
      auto &allEnts = s.scene.entities();

      bool found = false;
      for (auto &existingEnt : allEnts) {
        if (existingEnt.name == eData.name) {
          existingEnt.tr.pos = eData.pos;
          existingEnt.tr.rotDeg = eData.rot;
          existingEnt.tr.scale = eData.scale;
          found = true;
          break;
        }
      }

      if (!found) {
        // Optional: Create new entity if it doesn't exist?
        // For now, just warn.
        // std::cout << "Warning: Saved entity '" << eData.name << "' not found
        // in current scene.\n";
      }
    }
  }

  in.close();
  std::cout << "Loaded config (" << count << " entities) from " << filename
            << "\n";
}

void applyMouseLook(AppState &s, bool allowMouseLook) {
  if (allowMouseLook) {
    float xoff = (float)Mouse::getDX() * s.mouseSensitivity;
    float yoff = (float)Mouse::getDY() * s.mouseSensitivity;

    s.yaw += xoff;
    s.pitch += yoff;

    if (s.pitch > 89.0f)
      s.pitch = 89.0f;
    if (s.pitch < -89.0f)
      s.pitch = -89.0f;

    glm::vec3 front;
    front.x = cos(glm::radians(s.yaw)) * cos(glm::radians(s.pitch));
    front.y = sin(glm::radians(s.pitch));
    front.z = sin(glm::radians(s.yaw)) * cos(glm::radians(s.pitch));
    s.cameraFront = glm::normalize(front);
  } else {
    Mouse::getDX();
    Mouse::getDY();
  }
}

void processMovement(AppState &s) {
  bool shiftDown =
      Keyboard::key(GLFW_KEY_LEFT_SHIFT) || Keyboard::key(GLFW_KEY_RIGHT_SHIFT);
  float step = shiftDown ? (s.walkStep * s.runMult) : s.walkStep;

  glm::vec3 forward =
      glm::normalize(glm::vec3(s.cameraFront.x, 0.0f, s.cameraFront.z));
  glm::vec3 right = glm::normalize(glm::cross(forward, s.cameraUp));

  if (Keyboard::key(GLFW_KEY_W)) {
    s.x += forward.x * step;
    s.z += forward.z * step;
  }
  if (Keyboard::key(GLFW_KEY_S)) {
    s.x -= forward.x * step;
    s.z -= forward.z * step;
  }
  if (Keyboard::key(GLFW_KEY_D)) {
    s.x += right.x * step;
    s.z += right.z * step;
  }
  if (Keyboard::key(GLFW_KEY_A)) {
    s.x -= right.x * step;
    s.z -= right.z * step;
  }

  if (Keyboard::key(GLFW_KEY_UP))
    s.y += step;
  if (Keyboard::key(GLFW_KEY_DOWN))
    s.y -= step;

  bool spaceDown = Keyboard::key(GLFW_KEY_SPACE);
  if (spaceDown && !s.spaceWasDown && s.onGround) {
    s.yVel = s.jumpStrength;
    s.onGround = false;
  }
  s.spaceWasDown = spaceDown;
}

void updateJumpPhysics(AppState &s) {
  if (!s.onGround) {
    s.y += s.yVel;
    if (!s.freezePhysics)
      s.yVel -= s.gravity;

    if (s.y <= s.groundY) {
      s.y = s.groundY;
      s.yVel = 0.0f;
      s.onGround = true;
    }
  }
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

    s.scene.drawAllDepth(depthSh);
  }

  s.renderer.endShadowPass();
}

void renderMainPass(AppState &s, const glm::mat4 &view,
                    const glm::mat4 &projection, const glm::vec3 &cameraPos,
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

  s.scene.drawAll(s.renderer.shader());

  // 4. Draw FBX Model (Revert scale to 1.0f)
  s.myModel.draw(s.renderer.shader(), glm::vec3(5.0f, 0.0f, 5.0f),
                 glm::vec3(0.0f),
                 glm::vec3(1.0f) // REVERTED SCALE
  );

  s.projectiles.draw(view, projection, 0.25f);

  s.renderer.shader().activate();
  s.sun.draw(s.renderer.shader(), s.cameraFront, s.cameraUp);
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

  // ---- Scene setup ----
  m->treeId = m->scene.createEntityOBJ(
      "Tree", "/Users/edjan03/Downloads/glGen-main/assets/fbxmodels/test19/"
              "guard post in the middle of the forest.obj");
  m->carId =
      m->scene.createEntityOBJ("Car", "/Users/edjan03/Downloads/glGen-main/"
                                      "assets/tiger2/textures/sUntitled.obj");
  m->starwarsId = m->scene.createEntityOBJ(
      "StarWars",
      "/Users/edjan03/Downloads/star-wars-imperial-ii-star-destroyer/source/"
      "ImperialIIStarDetroyer.blend.obj");

  m->myModel.loadFromFile("/Users/edjan03/Downloads/glGen-main/assets/"
                          "adjustable wrench 3d model.glb");

  if (auto *sw = m->scene.get(m->starwarsId)) {
    sw->tr.pos = glm::vec3(8.0f, 0.0f, 8.0f);
    sw->tr.scale = glm::vec3(0.01f);
  }

  if (auto *tree = m->scene.get(m->treeId)) {
    tree->tr.pos = glm::vec3(5.0f, 0.0f, 5.0f);
    tree->tr.scale = glm::vec3(1.0f);
    if (tree->model)
      m->hasFireAnchor =
          tree->model->getSubmeshCenterLocal("Fire", m->fireLocal);
  }

  if (auto *car = m->scene.get(m->carId)) {
    car->tr.pos = glm::vec3(5.0f, 0.0f, 0.0f);
    car->tr.scale = glm::vec3(1.0f);

    if (car->model) {
      m->hasTurretAnchor = car->model->getSubmeshCenterLocal(
          "tank_guns_skinned", m->turretLocal);

      m->hasTurretPivot = car->model->getSubmeshCenterLocal(
          "tank_turret_02", m->turretPivotLocal);
      if (!m->hasTurretPivot)
        m->hasTurretPivot = car->model->getSubmeshCenterLocal(
            "tank_guns_skinned", m->turretPivotLocal);
    }
  }

  m->hasFire = m->hasFireAnchor;
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

    // Bridge EditorUI tree controls
    glm::vec3 dummyPos(0.0f), dummyScale(1.0f);
    glm::vec3 *treePosRef = &dummyPos;
    glm::vec3 *treeScaleRef = &dummyScale;

    if (auto *tree = m->scene.get(m->treeId)) {
      treePosRef = &tree->tr.pos;
      treeScaleRef = &tree->tr.scale;
    }

    EditorUIOutput uiOut = m->editor.draw(
        m->uiMode, m->walkStep, m->runMult, m->jumpStrength, m->gravity,
        m->freezePhysics, m->mouseSensitivity, m->fov, m->sun, m->fire,
        m->terrainSize, m->terrainSpacing, *treePosRef, *treeScaleRef);
    if (uiOut.saveRequested) {
      saveConfig(*m, "editor_state.bin");
    }
    if (uiOut.loadRequested) {
      loadConfig(*m, "editor_state.bin");
    }
    m->sky.setRotationDegrees(uiOut.skyRotDeg);

    drawSkyUI(*m);

    bool allowGameInput = (!m->uiMode && !uiOut.wantCaptureKeyboard);
    if (allowGameInput) {
      processMovement(*m);

      // Turret yaw (Q/E)
      float turretSpeed = 90.0f; // deg/sec
      if (Keyboard::key(GLFW_KEY_Q))
        m->turretYawDeg += turretSpeed * dt;
      if (Keyboard::key(GLFW_KEY_E))
        m->turretYawDeg -= turretSpeed * dt;
    }

    // Apply turret yaw
    if (auto *car = m->scene.get(m->carId)) {
      if (car->model) {
        car->model->setObjectYawDegPivot("Turret_02", m->turretYawDeg,
                                         m->turretPivotLocal);
        car->model->setObjectYawDegPivot("gun_05Shape5", m->turretYawDeg,
                                         m->turretPivotLocal);
      }
    }

    updateJumpPhysics(*m);

    glm::vec3 cameraPos(m->x, m->y, m->z);
    glm::mat4 view =
        glm::lookAt(cameraPos, cameraPos + m->cameraFront, m->cameraUp);

    int fbW, fbH;
    glfwGetFramebufferSize(m->window, &fbW, &fbH);
    glm::mat4 projection = glm::perspective(
        glm::radians(m->fov), (float)fbW / (float)fbH, 0.1f, 500.0f);

    bool allowMouseLook =
        (!m->uiMode && !uiOut.wantCaptureMouse && !ImGuizmo::IsUsing());
    applyMouseLook(*m, allowMouseLook);

    if (!m->uiMode && Mouse::buttonWentDown(GLFW_MOUSE_BUTTON_1) &&
        m->hasTurretAnchor) {
      if (auto *car = m->scene.get(m->carId)) {
        glm::mat4 TR(1.0f);
        TR = glm::translate(TR, car->tr.pos);
        TR =
            glm::rotate(TR, glm::radians(car->tr.rotDeg.y), glm::vec3(0, 1, 0));
        TR =
            glm::rotate(TR, glm::radians(car->tr.rotDeg.x), glm::vec3(1, 0, 0));
        TR =
            glm::rotate(TR, glm::radians(car->tr.rotDeg.z), glm::vec3(0, 0, 1));

        glm::mat4 S = glm::scale(glm::mat4(1.0f), car->tr.scale);

        glm::mat4 turretExtra(1.0f);
        if (m->hasTurretPivot) {
          turretExtra =
              glm::translate(glm::mat4(1.0f), m->turretPivotLocal) *
              glm::rotate(glm::mat4(1.0f), glm::radians(m->turretYawDeg),
                          glm::vec3(0, 1, 0)) *
              glm::translate(glm::mat4(1.0f), -m->turretPivotLocal);
        }

        glm::mat4 Mfire = TR * turretExtra * S;

        glm::vec3 muzzlePos =
            glm::vec3(Mfire * glm::vec4(m->turretLocal, 1.0f));
        glm::vec3 forward = glm::normalize(glm::vec3(Mfire[2]));
        glm::vec3 spawnPos = muzzlePos + forward * 1.2f;
        float smokeOffset = 2.35f;
        glm::vec3 smokePos = muzzlePos + forward * smokeOffset;

        m->projectiles.add(spawnPos, forward * 40.0f, 2.0f,
                           glm::vec3(1.0f, 0.8f, 0.2f));
        m->projectiles.addSmokeBurst(smokePos, forward);
      }
    }

    // NEW: Draw Gizmo / Outliner via EditorUI class
    {
      EditorSelectionState selState{m->selectedEntityIndex, m->selectedEntities,
                                    m->lastClickedEntity,   m->editObjPart,
                                    m->selectedObjPartName, (int &)m->gizmoOp,
                                    (int &)m->gizmoMode,    m->renaming,
                                    m->renameBuf,           m->outlinerFilter,
                                    m->focusDistance};

      glm::vec3 camPos(m->x, m->y, m->z);
      m->editor.drawGizmo(m->uiMode, view, projection, m->scene, m->sun,
                          selState, camPos);

      // Sync camera back (in case Focus was used)
      m->x = camPos.x;
      m->y = camPos.y;
      m->z = camPos.z;
    }

    bool editingSun =
        (m->uiMode && m->selectedEntityIndex == -1 && ImGuizmo::IsUsing());
    if (!editingSun)
      m->sun.update(dt, nowT);

    m->projectiles.update(dt);

    renderShadowPass(*m, m->sun.sunPos, 1.0f, 250.0f);
    renderMainPass(*m, view, projection, cameraPos, nowT);

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

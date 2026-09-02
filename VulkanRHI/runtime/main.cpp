// glGenVk — the real Vulkan runtime: a GLFW_NO_API window + Vulkan surface
// driving the engine's actual systems from EngineCore (Scene + AssetManager
// for assets, Jolt PhysicsSystem for simulation, ScriptSystem for Lua, the
// ECS for state) with VulkanRHI as the renderer. No OpenGL anywhere in the
// link.
//
// Startup goes through Engine/Core/SubsystemManager (the same GL-free
// dependency-ordered manager the old OpenGL app used), registering
// Vulkan-side subsystems (VulkanRHI/runtime/subsystems/) that own
// Window/Physics/Script/Audio/Editor over VkAppState. Gameplay (player
// controller/interaction, spaceship control) is driven by ported copies of
// the old Runtime/Gameplay systems (VulkanRHI/runtime/gameplay/). FX
// rendering (clouds/fire/volumetric fog/black hole) was never ported to
// Vulkan (skipped by explicit decision). A CPU-built terrain chunk is grown
// incrementally per TERRAIN_GENERATOR_PLAN.md (see VkTerrainSubsystem;
// Phase 1 is one fixed static chunk, no streaming yet).
// OpenGL has been fully removed from the repo -- this is the only renderer.
//
// The engine boots to an EMPTY scene with an editor grid. Terrain, props and
// a player are all things you create (Create menu / Terrain panel), not things
// the runtime assumes. GLGEN_DEMO_SCENE=1 and GLGEN_TERRAIN_ON_START=1 bring
// back content at startup for headless runs and quick visual checks.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VulkanContext.h"
#include "VulkanRenderSystem.h"
#include "VulkanRenderer.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include "EditorCamera.h"
#include "EditorGrid.h"
#include "VkEditor.h"
#include "ui/UIFonts.h"
#include "ui/UITheme.h"
#include "ui/UIWidgets.h"

// EngineCore — the engine's real, GL-free systems.
#include "Assets/AssetManager.h"
#include "Assets/MeshData.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Keyboard.h"
#include "Logger.h"
#include "Mouse.h"

#include "VkAgentBridge.h"
#include "VkAppState.h"
#include "subsystems/VkAudioSubsystem.h"
#include "subsystems/VkCoreAppLayer.h"
#include "subsystems/VkEditorSubsystem.h"
#include "subsystems/VkPhysicsSubsystem.h"
#include "subsystems/VkScriptSubsystem.h"
#include "subsystems/VkTerrainSubsystem.h"
#include "subsystems/VkWindowSubsystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {
// Accumulated scroll, drained each frame. Installed before ImGui so ImGui's
// own callback chains to it. (Independent of the Keyboard/Mouse statics --
// this feeds EditorCamera's dolly zoom, not gameplay.)
double g_scrollY = 0.0;
void scrollCallback(GLFWwindow *, double, double yoffset) {
  g_scrollY += yoffset;
}

// Phase 0 verification hook for AI_ASSET_PIPELINE_PLAN.md: a MeshData built
// entirely in memory, with no file behind it. This is the shape every future
// generator (tree.v1, rock.v1, ...) will emit -- it exists here so the
// register -> render -> regenerate path stays exercised as those land.
// An axis-aligned box, base at y=0, centered in XZ.
std::unique_ptr<MeshData> makeGeneratedBox(glm::vec3 size, glm::vec3 color) {
  auto data = std::make_unique<MeshData>();
  data->sourcePath = "gen://phase0/box";

  MeshSubmeshData sm;
  sm.objectName = "GeneratedBox";
  sm.materialName = "generated";
  sm.debugName = "GeneratedBox";
  sm.material.baseColor = glm::vec4(color, 1.0f);
  sm.material.roughness = 0.6f;

  const glm::vec3 h(size.x * 0.5f, size.y, size.z * 0.5f);
  // Per-face corner offsets (CCW seen from outside) + that face's normal.
  const glm::vec3 faces[6][4] = {
      {{-1, 0, 1}, {1, 0, 1}, {1, 1, 1}, {-1, 1, 1}},     // +Z
      {{1, 0, -1}, {-1, 0, -1}, {-1, 1, -1}, {1, 1, -1}}, // -Z
      {{1, 0, 1}, {1, 0, -1}, {1, 1, -1}, {1, 1, 1}},     // +X
      {{-1, 0, -1}, {-1, 0, 1}, {-1, 1, 1}, {-1, 1, -1}}, // -X
      {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},   // +Y
      {{-1, 0, -1}, {1, 0, -1}, {1, 0, 1}, {-1, 0, 1}},   // -Y
  };
  const glm::vec3 normals[6] = {{0, 0, 1},  {0, 0, -1}, {1, 0, 0},
                                {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}};
  const glm::vec2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

  for (int f = 0; f < 6; ++f) {
    const uint32_t base = static_cast<uint32_t>(sm.vertices.size());
    for (int c = 0; c < 4; ++c) {
      MeshVertex v;
      v.pos = faces[f][c] * h;
      v.uv = uvs[c];
      v.normal = normals[f];
      sm.vertices.push_back(v);
    }
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
      sm.indices.push_back(base + i);
  }

  for (const auto &v : sm.vertices) {
    sm.aabbMin = glm::min(sm.aabbMin, v.pos);
    sm.aabbMax = glm::max(sm.aabbMax, v.pos);
    sm.hasBounds = true;
  }

  MeshObjectBounds ob{sm.aabbMin, sm.aabbMax, true};
  data->objectBounds.emplace_back(sm.objectName, ob);
  data->submeshes.push_back(std::move(sm));
  return data;
}

// Scales an entity so its mesh's largest authored dimension spans
// `targetSize` world units, and returns that uniform scale.
float normalizeEntityScale(Registry &reg, AssetManager &assets, EntityId e,
                           float targetSize) {
  if (!reg.has<MeshComponent>(e) || !reg.has<TransformComponent>(e))
    return 1.0f;
  MeshComponent &mc = reg.get<MeshComponent>(e);
  const MeshData *data = assets.getOBJData(mc.objHandle);
  glm::vec3 mn, mx;
  if (!data || !data->getGlobalBounds(mn, mx))
    return 1.0f;
  const glm::vec3 size = mx - mn;
  const float maxDim = std::max(size.x, std::max(size.y, size.z));
  const float s = (maxDim > 1e-5f) ? targetSize / maxDim : 1.0f;
  reg.get<TransformComponent>(e).scale = glm::vec3(s);
  std::fprintf(stderr,
               "[glGenVk] '%s' bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f) "
               "maxDim %.2f -> scale %.3f\n",
               mc.assetId.c_str(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, maxDim,
               s);
  return s;
}
} // namespace

#ifndef GLGEN_VK_SHADER_DIR
#define GLGEN_VK_SHADER_DIR "."
#endif
#ifndef GLGEN_VK_ASSET_DIR
#define GLGEN_VK_ASSET_DIR "assets"
#endif
#ifndef GLGEN_VK_SCRIPT_DIR
#define GLGEN_VK_SCRIPT_DIR "scripts"
#endif

int main() {
  if (!glfwInit()) {
    std::fprintf(stderr, "[glGenVk] glfwInit failed\n");
    return 1;
  }
  if (!glfwVulkanSupported()) {
    std::fprintf(stderr, "[glGenVk] GLFW reports Vulkan not supported\n");
    glfwTerminate();
    return 1;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  // Start maximized (windowed, not exclusive fullscreen) so the editor has
  // room to breathe on launch; the user can still un-maximize normally.
  glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
  GLFWwindow *window =
      glfwCreateWindow(1280, 720, "glGen (Vulkan)", nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "[glGenVk] glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  uint32_t glfwExtCount = 0;
  const char **glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
  std::vector<const char *> instanceExtensions(glfwExts,
                                               glfwExts + glfwExtCount);

  vkrhi::VulkanContext ctx;
  vkrhi::VulkanContext::CreateInfo ci;
  ci.instanceExtensions = instanceExtensions;
  ci.enableValidation = true;
  if (!ctx.createInstance(ci)) {
    std::fprintf(stderr, "[glGenVk] createInstance failed\n");
    return 1;
  }

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(ctx.instance(), window, nullptr, &surface) !=
      VK_SUCCESS) {
    std::fprintf(stderr, "[glGenVk] glfwCreateWindowSurface failed\n");
    return 1;
  }

  if (!ctx.selectAndCreateDevice(surface)) {
    std::fprintf(stderr, "[glGenVk] device selection failed\n");
    return 1;
  }

  vkrhi::VulkanRenderer renderer;
  auto queryFbSize = [window](uint32_t &w, uint32_t &h) {
    int iw = 0, ih = 0;
    glfwGetFramebufferSize(window, &iw, &ih);
    w = static_cast<uint32_t>(iw);
    h = static_cast<uint32_t>(ih);
  };
  // The asset dir is passed so the renderer can load the volumetric cloud
  // noise (assets/clouds/). Everything else it needs still arrives through
  // the scene API.
  if (!renderer.init(ctx, surface, GLGEN_VK_SHADER_DIR, queryFbSize,
                     GLGEN_VK_ASSET_DIR)) {
    std::fprintf(stderr, "[glGenVk] renderer init failed\n");
    return 1;
  }

  // --- the engine: EngineCore systems, no OpenGL --------------------------
  VkAppState state;
  state.window = window;
  state.scene.setAssetManager(&state.assets);
  Registry &reg = state.scene.registry();

  const std::string assetDir = GLGEN_VK_ASSET_DIR;
  state.assetDir = assetDir;

  // Procedurally generated assets (AI_ASSET_PIPELINE_PLAN.md Phase 1): every
  // *.json under assets/recipes is run through its generator and registered
  // with the AssetManager, ready to be referenced by MeshComponent::assetId.
  state.assetLibrary.initialize(state.assets, assetDir + "/recipes");
  {
    std::vector<std::string> messages;
    const int loaded = state.assetLibrary.loadAll(messages);
    std::fprintf(stderr, "[glGenVk] recipes: %d generated from %s/recipes\n",
                 loaded, assetDir.c_str());
    for (const std::string &m : messages)
      std::fprintf(stderr, "[glGenVk]   %s\n", m.c_str());
  }
  // NO DEMO SCENE. The engine boots empty: an editor whose runtime hardcodes
  // its own content is a demo, and every launch used to rebuild the same two
  // trees, an axe, six falling rocks and a scripted player whether you wanted
  // them or not. Terrain comes from Create > Terrain, props from the Create
  // menu or the Assets panel, and a player from Create > Player.
  //
  // GLGEN_DEMO_SCENE=1 restores a minimal version for anyone who wants
  // something in front of the camera immediately.
  if (std::getenv("GLGEN_DEMO_SCENE")) {
    // Baked by Tools/glgen-bake from assets/custom_trees/, like the scatter
    // layers use. Authored Y-up, so no corrective rotation here -- the old
    // tree.obj needed a 90-degree fix because its trunk ran along local X.
    const std::string treePath = assetDir + "/trees/conifer_tall.obj";
    for (const glm::vec3 &pos :
         {glm::vec3(-1.6f, 0.0f, -1.2f), glm::vec3(1.9f, 0.0f, 1.4f)}) {
      EntityId e = state.scene.spawnFromFile(treePath);
      if (e == 0)
        continue;
      state.assets.recenterOBJ(reg.get<MeshComponent>(e).objHandle,
                               MeshData::Recenter::BaseY);
      reg.get<TransformComponent>(e).position = pos;
      normalizeEntityScale(reg, state.assets, e, 1.6f);
    }
  }

  vkrhi::VulkanRenderSystem renderSystem;
  renderSystem.setAssets(&state.assets);

  VkEditor editor; // the old engine's editor shell, Vulkan-side

  state.renderer = &renderer;
  state.renderSystem = &renderSystem;
  state.vkEditor = &editor;

  // --- Dear ImGui (platform: GLFW, renderer: Vulkan, dynamic rendering) ---
  VkDescriptorPool imguiPool = VK_NULL_HANDLE;
  {
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = 16;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(ctx.device(), &pci, nullptr, &imguiPool);
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  // Own layout file so the GL editor's imgui.ini isn't clobbered (and its
  // stale window positions aren't inherited).
  ImGui::GetIO().IniFilename = "imgui_glgenvk.ini";
  // Tab/arrow navigation through panels -- the keyboard's job in editor
  // mode is UI, not movement.
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // New editor look: fonts first (the runtime used to load none at all and
  // rendered everything in the 13px bitmap default), then the style.
  UIFonts::load(assetDir);
  UITheme::apply();

  // Gameplay input (ScriptBindings' Lua input.* API and
  // VkPlayerControllerSystem/VkPlayerInteractionSystem read these statics
  // directly) -- installed before ImGui so its own GLFW backend chains onto
  // them, same as the pre-existing scroll callback below.
  glfwSetKeyCallback(window, Keyboard::keyCallBack);
  glfwSetCursorPosCallback(window, Mouse::cursorPosCallback);
  glfwSetMouseButtonCallback(window, Mouse::mouseButtonCallback);
  glfwSetScrollCallback(window, scrollCallback); // before ImGui so it chains
  ImGui_ImplGlfw_InitForVulkan(window, true);

  static VkFormat colorFormat = renderer.swapchainColorFormat();
  ImGui_ImplVulkan_InitInfo initInfo{};
  initInfo.ApiVersion = VK_API_VERSION_1_3;
  initInfo.Instance = ctx.instance();
  initInfo.PhysicalDevice = ctx.physicalDevice();
  initInfo.Device = ctx.device();
  initInfo.QueueFamily = ctx.graphicsQueueFamily();
  initInfo.Queue = ctx.graphicsQueue();
  initInfo.DescriptorPool = imguiPool;
  initInfo.MinImageCount = 2;
  initInfo.ImageCount = renderer.swapchainImageCount();
  initInfo.UseDynamicRendering = true;
  // Since ImGui 1.92: MSAA + dynamic-rendering formats live in PipelineInfoMain.
  initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = {};
  initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats =
      &colorFormat;
  ImGui_ImplVulkan_Init(&initInfo);

  renderer.setOverlayCallback([](VkCommandBuffer cmd) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
  });

  // --- subsystem startup: Window -> Physics -> Script/Audio -> Editor -----
  auto audioOwned = std::make_unique<VkAudioSubsystem>(state);
  VkAudioSubsystem *audio = audioOwned.get();
  auto terrainOwned = std::make_unique<VkTerrainSubsystem>(state);
  VkTerrainSubsystem *terrain = terrainOwned.get();
  state.terrainSubsystem = terrain;
  state.subsystems.registerSubsystem(std::make_unique<VkWindowSubsystem>(state));
  state.subsystems.registerSubsystem(std::make_unique<VkPhysicsSubsystem>(state));
  state.subsystems.registerSubsystem(std::move(terrainOwned));
  state.subsystems.registerSubsystem(std::make_unique<VkScriptSubsystem>(state));
  state.subsystems.registerSubsystem(std::move(audioOwned));
  state.subsystems.registerSubsystem(std::make_unique<VkEditorSubsystem>(state));
  if (!state.subsystems.initializeAll()) {
    std::fprintf(stderr, "[glGenVk] subsystem startup failed\n");
    return 1;
  }

  // Command port: OFF unless a port is given. Anything that can connect can
  // run arbitrary Lua in this process, so it is opt-in and loopback-only.
  VkAgentBridge agentBridge;
  {
    // On by default now. It used to be opt-in, which meant opening the editor
    // normally and then pointing an AI client at it silently started a SECOND
    // engine instance -- the client edited a window you were not looking at.
    // Loopback-only, and GLGEN_AGENT_PORT=0 turns it off.
    const char *portEnv = std::getenv("GLGEN_AGENT_PORT");
    const int port = portEnv ? std::atoi(portEnv) : 8787;
    if (port == 0) {
      std::fprintf(stderr, "[glGenVk] agent bridge disabled\n");
    } else if (port > 0 && port < 65536) {
      if (agentBridge.start(state, static_cast<uint16_t>(port)))
        std::fprintf(stderr, "[glGenVk] agent bridge on 127.0.0.1:%d\n", port);
      else
        std::fprintf(stderr, "[glGenVk] agent bridge failed: %s\n",
                     agentBridge.server().lastError().c_str());
    } else {
      std::fprintf(stderr, "[glGenVk] GLGEN_AGENT_PORT '%s' is not a port\n",
                   portEnv);
    }
  }

  std::fprintf(stderr, "[glGenVk] Running. Close the window to exit.\n");

  int maxFrames = 0;
  if (const char *framesEnv = std::getenv("GLGEN_SMOKE_FRAMES"))
    maxFrames = std::atoi(framesEnv);
  const char *capturePath = std::getenv("GLGEN_SMOKE_CAPTURE");
  if (capturePath && maxFrames <= 0)
    maxFrames = 8;

  // A slightly raised viewpoint over the drop zone. The terrain's actual
  // height at the drop zone varies with seed/settings (procedural noise, no
  // flattened spawn area), so the hardcoded XZ is placed just above the
  // real generated surface via TerrainQuery rather than a fixed Y that used
  // to assume near-0 height there.
  {
    vkrhi::VulkanRenderer::Params &p = renderer.params();
    if (terrain->hasTerrain()) {
      // Standing on the ground, wherever the generator put it.
      p.camPos = glm::vec3(0.0f, 1.1f, 3.4f);
      p.camPos.y = terrain->heightAt(glm::vec2(p.camPos.x, p.camPos.z)) + 1.8f;
      p.camPitchDeg = -14.0f;
    } else {
      // Empty scene: a three-quarter view down at the origin, far enough out
      // that a few grid squares are visible and anything created at 0,0,0 is
      // already in frame.
      p.camPos = glm::vec3(6.0f, 4.5f, 8.0f);
      p.camYawDeg = 216.0f;
      p.camPitchDeg = -22.0f;
    }

    // The player entity was created earlier (before terrain existed to
    // query) with the same hardcoded XZ and a placeholder Y -- correct it
    // now the same way, or a large heightScale (or any seed where the drop
    // zone isn't near height 0) spawns the player buried in or floating far
    // above the terrain. VkEditor's Play button snapshots whatever the
    // scene looks like right now, so this is the position every future
    // Play press restores to -- it has to be right once, here.
    if (state.gameplay.playerId != 0) {
      Registry &reg = state.scene.registry();
      if (reg.has<TransformComponent>(state.gameplay.playerId))
        reg.get<TransformComponent>(state.gameplay.playerId).position =
            p.camPos;
    }
    if (std::getenv("GLGEN_SMOKE_DUSK"))
      p.lightPitchDeg = 8.0f;
    // Pulls the camera back and raises fog density so the terrain/sky
    // horizon seam (or its absence) is actually visible in a capture --
    // the default near-origin scene view rarely reaches fogParams.z.
    if (std::getenv("GLGEN_SMOKE_FOGTEST")) {
      p.camPos = glm::vec3(0.0f, 8.0f, 40.0f);
      p.camPitchDeg = -6.0f;
      p.fogDensity = 0.02f;
      p.fogStart = 5.0f;
    }
    // High, angled overview of the streamed chunks (up to ~192m across at
    // the default viewDistanceChunks) -- the near-origin drop-zone view is
    // mostly flat, so slope-dependent material work (rock bands, triplanar
    // detail) needs a wider vantage to actually show anything.
    if (std::getenv("GLGEN_SMOKE_SURVEY")) {
      p.camPos = glm::vec3(0.0f, 60.0f, 60.0f);
      p.camPitchDeg = -45.0f;
      p.fogDensity = 0.0025f;
    }
    if (const char *pose = std::getenv("GLGEN_SMOKE_STYLE_POSE")) {
      p.lightYawDeg = 215.0f;
      p.lightPitchDeg = 27.0f;
      p.autoExposure = false;
      p.exposure = 0.9f;
      if (std::strcmp(pose, "forest") == 0) {
        p.camPos = glm::vec3(-18.0f, 0.0f, 24.0f);
        p.camPos.y = terrain->heightAt(glm::vec2(p.camPos.x, p.camPos.z)) + 3.0f;
        p.camYawDeg = 150.0f;
        p.camPitchDeg = -5.0f;
      } else if (std::strcmp(pose, "mountain") == 0) {
        p.camPos = glm::vec3(32.0f, 0.0f, 72.0f);
        p.camPos.y = terrain->heightAt(glm::vec2(p.camPos.x, p.camPos.z)) + 55.0f;
        p.camYawDeg = 205.0f;
        p.camPitchDeg = -24.0f;
      } else {
        p.camPos = glm::vec3(0.0f, 0.0f, 42.0f);
        p.camPos.y = terrain->heightAt(glm::vec2(p.camPos.x, p.camPos.z)) + 6.0f;
        p.camYawDeg = 180.0f;
        p.camPitchDeg = -8.0f;
      }
    }
    // Straight-down bird's-eye at an altitude scaled to the streamed radius
    // (viewDistanceChunks * chunkWorldSize), fog nearly off -- for actually
    // SEEING how much of the streamed disc is loaded/rendered, which
    // GLGEN_SMOKE_SURVEY's fixed near-ground vantage can't show (atmospheric
    // fog legitimately hides most of a large radius from there).
    if (const char *aerial = std::getenv("GLGEN_SMOKE_AERIAL")) {
      const float radius = static_cast<float>(std::atof(aerial));
      const float altitude = std::max(radius * 1.2f, 100.0f);
      p.camPos = glm::vec3(0.0f, altitude, 0.0f);
      p.camPitchDeg = -89.0f;
      p.fogDensity = 0.0002f;
      p.fogMaxOpacity = 0.15f;
    }
    // Eye-level close-up of the procedural grass material
    // (grassMaterial.glsl), with the grass band widened to cover the whole
    // drop zone so the capture is wall-to-wall meadow regardless of local
    // terrain height. Blade/clump/scatter layers are all inside their full-
    // detail LOD range at this distance.
    if (std::getenv("GLGEN_SMOKE_GRASS")) {
      p.camPos = glm::vec3(0.0f, 1.4f, 6.0f);
      p.camPitchDeg = -18.0f;
      p.grassStart = -50.0f;
      p.grassEnd = -49.0f;
    }
    // Sun below the horizon (distinct from GLGEN_SMOKE_DUSK's grazing
    // angle), for auto-exposure's night-target end of the curve.
    if (std::getenv("GLGEN_SMOKE_NIGHT"))
      p.lightPitchDeg = -30.0f;
    // Camera looking INTO the low sun (disc, transmittance color, Mie glow)
    // -- the default views all face away from the sun, which left the whole
    // sun-ward look unverifiable. lightYawDeg is the direction light
    // TRAVELS: yaw 0 sends light toward +Z, so the sun ITSELF sits toward
    // -Z, in front of the -Z-facing default camera. Camera raised clear of
    // the drop-zone hillside.
    if (std::getenv("GLGEN_SMOKE_SUNVIEW")) {
      p.lightYawDeg = 0.0f;
      p.lightPitchDeg = 11.0f;
      p.camPos = glm::vec3(0.0f, 60.0f, 60.0f);
      p.camPitchDeg = 4.0f;
    }
    // Camera looking at the risen full moon (the moon is modeled antipodal:
    // it sits along the sun's travel direction, so yaw 180 / negative pitch
    // puts it toward -Z, above the horizon, sun well below).
    if (std::getenv("GLGEN_SMOKE_MOONVIEW")) {
      p.lightYawDeg = 180.0f;
      p.lightPitchDeg = -35.0f;
      p.camPos = glm::vec3(0.0f, 60.0f, 60.0f);
      p.camPitchDeg = 24.0f;
    }
    // Camera pitched up into open sky, sun off to one side -- the pose the
    // volumetric cloud layer actually needs, since every other preset here
    // points at terrain and shows a sliver of sky at most. Value is the pitch
    // in degrees (default 22); the sun sits ~35 degrees to the left of the
    // view so lit tops and shadowed bases are both visible in one frame.
    if (const char *skyview = std::getenv("GLGEN_SMOKE_SKYVIEW")) {
      const float pitch = skyview[0] ? static_cast<float>(std::atof(skyview)) : 22.0f;
      p.lightYawDeg = 35.0f;
      p.lightPitchDeg = 24.0f;
      // High enough to clear the tree canopy: at 60 m the scattered pines
      // fill most of an upward-pitched frame and there is barely any sky to
      // judge.
      p.camPos = glm::vec3(0.0f, 260.0f, 60.0f);
      p.camYawDeg = 180.0f;
      p.camPitchDeg = pitch;
      // Auto-exposure would chase the cloud cover: a mostly-clouded frame
      // pushes exposure up and blows the clear sky out, so two captures of
      // the same sky are not comparable. Pin it.
      p.autoExposure = false;
      p.exposure = 1.0f;
    }
    // Neutralizes the painterly sky wash (band quantization + palette pull).
    // It is the engine's house look and it is applied to clouds too, which is
    // correct -- but it makes the volumetric layer's own lighting impossible
    // to judge, so this turns it off for a diagnostic capture.
    if (std::getenv("GLGEN_SMOKE_NOWASH"))
      p.style.skyGradeStrength = 0.0f;
    if (std::getenv("GLGEN_SMOKE_AUTOEXP"))
      p.autoExposure = true;
    // Force a debug view mode for a capture (e.g. 6 = NaN detector -- any
    // magenta pixel means broken shader math somewhere upstream).
    if (const char *dbg = std::getenv("GLGEN_SMOKE_DEBUGVIEW"))
      p.debugViewMode = std::atoi(dbg);
    // Ground-optics A/B: the additive grass optics are calibrated to be a
    // fraction of direct sun, and the failure mode they had (scaling with
    // raw HDR sun luminance instead) is reproducible by setting this to ~9.
    // Exposed headlessly so that regression stays measurable from a capture.
    if (const char *ex = std::getenv("GLGEN_SMOKE_EXPOSURE"))
      p.exposure = static_cast<float>(std::atof(ex));
    // Sun/sky isolation. Both light sources vanish at night, so "it looks
    // right at night" cannot on its own tell you which one is blowing out
    // the ground -- these two let each be killed independently.
    if (const char *si = std::getenv("GLGEN_SMOKE_SUNINTENSITY"))
      p.sunIntensity = static_cast<float>(std::atof(si));
    if (const char *sk = std::getenv("GLGEN_SMOKE_SKYINTENSITY")) {
      const float v = static_cast<float>(std::atof(sk));
      p.ambientIntensity = v;          // diffuse sky irradiance
      p.iblSpecularIntensity = v;      // specular sky reflection
      p.terrainSkyReflectIntensity = v * 0.2f;
    }
    // "x,y,z,pitchDeg[,yawDeg]" -- arbitrary camera placement, so a look
    // reported from the editor can be reproduced headlessly instead of
    // approximated with the nearest preset.
    if (const char *cam = std::getenv("GLGEN_SMOKE_CAM")) {
      float v[5] = {p.camPos.x, p.camPos.y, p.camPos.z, p.camPitchDeg, p.camYawDeg};
      int n = std::sscanf(cam, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
      if (n >= 4) {
        p.camPos = glm::vec3(v[0], v[1], v[2]);
        p.camPitchDeg = v[3];
        if (n >= 5)
          p.camYawDeg = v[4];
      }
    }
    // Fog fully off, matching "I removed the fog" -- note this does NOT
    // disable mountain aerial perspective, which is a separate term.
    if (std::getenv("GLGEN_SMOKE_NOFOG")) {
      p.fogDensity = 0.0f;
      p.fogMaxOpacity = 0.0f;
    }
  }

  // Phase 0 of AI_ASSET_PIPELINE_PLAN.md, end to end: build a mesh in memory,
  // register it under a "gen://" id with no file behind it, and spawn an
  // entity for it. GLGEN_GEN_TEST=<frame> additionally regenerates the asset
  // at that frame (AssetManager::replaceMeshData) to prove the render system
  // notices the content-version change and re-uploads in place -- the
  // mechanism every generator will rely on for hot reload.
  const char *genTestEnv = std::getenv("GLGEN_GEN_TEST");
  const std::string kGenAssetId = "gen://phase0/box";
  int genSwapFrame = 0;
  if (genTestEnv) {
    genSwapFrame = std::atoi(genTestEnv);
    if (genSwapFrame <= 0)
      genSwapFrame = 6;

    vkrhi::VulkanRenderer::Params &p = renderer.params();
    // 4 m in front of the camera along its view direction, sitting on the
    // terrain surface.
    const float yaw = glm::radians(p.camYawDeg);
    const glm::vec2 spotXZ(p.camPos.x + std::sin(yaw) * 4.0f,
                           p.camPos.z + std::cos(yaw) * 4.0f);

    state.assets.registerMeshData(
        kGenAssetId,
        makeGeneratedBox(glm::vec3(1.2f, 0.6f, 1.2f), // short and wide
                         glm::vec3(0.75f, 0.15f, 0.12f)));

    EntityId e = reg.create();
    auto &t = reg.emplace<TransformComponent>(e);
    t.position = glm::vec3(spotXZ.x, terrain->heightAt(spotXZ), spotXZ.y);
    reg.emplace<MeshComponent>(e).assetId = kGenAssetId;
    reg.emplace<NameComponent>(e, NameComponent("GeneratedBox"));
    std::fprintf(stderr,
                 "[glGenVk] gen-test: spawned '%s' at (%.2f %.2f %.2f); "
                 "regenerating at frame %d\n",
                 kGenAssetId.c_str(), t.position.x, t.position.y, t.position.z,
                 genSwapFrame);
  }

  // Asset review rig: lines every loaded recipe up in a row and frames the
  // whole row against open sky. The altitude is deliberate -- reviewing a
  // generated asset means reading its SILHOUETTE, and at ground level the
  // streamed forest and grass occlude exactly what needs checking. This is a
  // minimal version of the fixed-framing turntable the plan calls for
  // (AI_ASSET_PIPELINE_PLAN.md §3.4): same camera, same light, every time, so
  // two captures are actually comparable.
  //
  // GLGEN_GEN_SHOWCASE=<recipe-id-substring> narrows it to matching recipes
  // (e.g. "tree" for just the trees); "1" or empty shows everything.
  if (const char *showcase = std::getenv("GLGEN_GEN_SHOWCASE")) {
    const std::string filter =
        (std::strcmp(showcase, "1") == 0) ? std::string{} : std::string(showcase);

    std::vector<std::string> ids;
    for (const std::string &id : state.assetLibrary.recipeIds())
      if (filter.empty() || id.find(filter) != std::string::npos)
        ids.push_back(id);

    if (!ids.empty()) {
      const float spacing = 5.0f;
      const float rowWidth = spacing * static_cast<float>(ids.size() - 1);
      const float altitude = terrain->heightAt(glm::vec2(0.0f)) + 30.0f;
      const float start = -0.5f * rowWidth;

      for (size_t i = 0; i < ids.size(); ++i) {
        const std::string assetId = state.assetLibrary.assetIdOfRecipe(ids[i]);
        if (assetId.empty())
          continue;
        EntityId e = reg.create();
        reg.emplace<TransformComponent>(e).position = glm::vec3(
            start + spacing * static_cast<float>(i), altitude, 0.0f);
        reg.emplace<MeshComponent>(e).assetId = assetId;
        reg.emplace<NameComponent>(e, NameComponent(ids[i]));
        std::fprintf(stderr, "[glGenVk] showcase: '%s' -> %s\n", ids[i].c_str(),
                     assetId.c_str());
      }

      // Frame the row: back off far enough for its full width to fit the
      // horizontal FOV, with a little headroom.
      vkrhi::VulkanRenderer::Params &p = renderer.params();
      const float halfFov = glm::radians(p.fovDeg * 0.5f);
      const float distance =
          std::max(12.0f, (rowWidth * 0.62f) / std::tan(halfFov));
      p.camPos = glm::vec3(0.0f, altitude + 4.0f, distance);
      p.camYawDeg = 180.0f; // look toward -Z, at the row
      p.camPitchDeg = -4.0f;
      p.fogDensity = 0.0f;
      p.fogMaxOpacity = 0.0f;
      p.autoExposure = false;
      p.exposure = 0.9f;
      p.lightYawDeg = 215.0f;
      p.lightPitchDeg = 38.0f;
      // EditorCamera is constructed below and seeds itself from these params.
    }
  }

  // Runs a Lua file once at startup, through the same VM and API the editor
  // Console drives. The headless counterpart of typing into that prompt, and
  // the way a scripted scene setup is exercised without a mouse.
  if (const char *scriptPath = std::getenv("GLGEN_SCRIPT")) {
    std::ifstream in(scriptPath);
    if (!in.is_open()) {
      std::fprintf(stderr, "[glGenVk] GLGEN_SCRIPT: cannot open '%s'\n",
                   scriptPath);
    } else {
      const std::string source((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
      std::string error;
      if (state.scriptSystem.execString(source, error))
        std::fprintf(stderr, "[glGenVk] GLGEN_SCRIPT: ran '%s'\n", scriptPath);
      else
        std::fprintf(stderr, "[glGenVk] GLGEN_SCRIPT error: %s\n",
                     error.c_str());
    }
  }

  // Headless streaming exercise: fly the camera forward at a constant speed
  // so smoke runs actually hit chunk load/unload, LOD refresh, and mesh/BLAS
  // eviction (a static camera exercises none of those paths). Value = speed
  // in m/s ("1" gets a sensible default).
  float smokeFlySpeed = 0.0f;
  if (const char *fly = std::getenv("GLGEN_SMOKE_FLY")) {
    smokeFlySpeed = static_cast<float>(std::atof(fly));
    if (smokeFlySpeed <= 1.0f)
      smokeFlySpeed = 24.0f;
  }

  bool simulate = true;
  int frame = 0;

  // Editor viewport camera: mouse-only, smoothed, distance-adaptive (see
  // EditorCamera.h). Seeded from wherever the startup/smoke code above put
  // the renderer camera.
  EditorCamera editorCam;
  editorCam.seed(renderer.params().camPos, renderer.params().camYawDeg,
                 renderer.params().camPitchDeg);

  // Ground grid + origin axes, rebuilt every frame because the patch follows
  // the camera (see EditorGrid.h).
  EditorGridSettings gridSettings;
  std::vector<vkrhi::VulkanRenderer::DebugLineVertex> debugLines;
  // Pose to restore when leaving play mode -- Stop reverts the scene from
  // its snapshot, so the camera going back to its pre-play pose keeps
  // "Stop undoes everything" consistent.
  glm::vec3 editPosePos = renderer.params().camPos;
  float editPoseYaw = renderer.params().camYawDeg;
  float editPosePitch = renderer.params().camPitchDeg;

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    static double prevTime = glfwGetTime();
    const double nowTime = glfwGetTime();
    float dt = static_cast<float>(nowTime - prevTime);
    prevTime = nowTime;

    if (smokeFlySpeed > 0.0f) {
      vkrhi::VulkanRenderer::Params &p = renderer.params();
      const float yaw = glm::radians(p.camYawDeg);
      p.camPos += glm::vec3(std::sin(yaw), 0.0f, std::cos(yaw)) *
                  (smokeFlySpeed * std::min(dt, 0.1f));
      p.camPos.y =
          terrain->heightAt(glm::vec2(p.camPos.x, p.camPos.z)) + 12.0f;
    }
    dt = std::min(dt, 1.0f / 30.0f); // clamp for physics stability

    ImGuiIO &io = ImGui::GetIO();
    vkrhi::VulkanRenderer::Params &p = renderer.params();

    if (p.timeOfDayEnabled) {
      p.lightPitchDeg += p.timeOfDaySpeed * dt;
      if (p.lightPitchDeg > 180.0f)
        p.lightPitchDeg -= 360.0f;
      else if (p.lightPitchDeg < -180.0f)
        p.lightPitchDeg += 360.0f;
    }

    // Editor mode vs play mode is a hard split. Editor mode: the mouse
    // navigates the free-fly camera (RMB look / MMB pan / scroll zoom) and
    // the keyboard belongs to the UI only -- nothing in the world moves
    // from a keypress. Play mode (VkEditor's "> Play" button): the player
    // character owns camera, mouse-look and WASD, exactly like Stop/Play in
    // a real game engine. One frame of lag on this flag (editor.draw() runs
    // at the end of the frame) is harmless -- same as ctx.simulatePhysics
    // already tolerates.
    const bool inPlayMode = editor.isInPlayMode();
    static bool wasInPlayMode = false;
    if (inPlayMode != wasInPlayMode) {
      glfwSetInputMode(window, GLFW_CURSOR,
                       inPlayMode ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
      if (inPlayMode) {
        // Entering play: remember the editor camera pose.
        editPosePos = p.camPos;
        editPoseYaw = p.camYawDeg;
        editPosePitch = p.camPitchDeg;
        // The controller's fall velocity/grounded flag are its own members,
        // not part of the scene snapshot -- without this, stale downward
        // velocity from the tail end of a previous Play session (e.g. Stop
        // pressed mid-fall) carries into the next one, so the player can
        // start already "falling" through terrain that hasn't streamed a
        // physics collider in yet at the snapshot-restored spawn point.
        state.playerController.reset();
      } else {
        // Leaving play: Stop reverted the scene from its snapshot; put the
        // camera back too, and drop any in-flight grab so a stale entity id
        // can't be re-grabbed next session.
        p.camPos = editPosePos;
        p.camYawDeg = editPoseYaw;
        p.camPitchDeg = editPosePitch;
        editorCam.seed(editPosePos, editPoseYaw, editPosePitch);
        state.gameplay.grab = GrabState{};
      }
      wasInPlayMode = inPlayMode;
    }

    // Keep the engine-side play state in sync (footstep audio and Lua's
    // play-state gating read this; it used to be stuck on Playing forever,
    // which is why WASD triggered footstep sounds in editor mode).
    state.playState = !inPlayMode ? VkAppState::PlayState::Stopped
                     : (simulate ? VkAppState::PlayState::Playing
                                 : VkAppState::PlayState::Paused);

    // Mouse-only viewport navigation, editor-style: RMB drag = look, MMB
    // drag = pan, scroll = dolly zoom with momentum. All three combine
    // (scroll while RMB-looking works). Zoom/pan speed scales with height
    // above the terrain and the motion is exponentially smoothed -- see
    // EditorCamera.h. Skipped during headless smoke-fly runs, which drive
    // p.camPos directly.
    if (!inPlayMode && smokeFlySpeed <= 0.0f) {
      static bool looking = false;
      static bool panning = false;
      static double lastX = 0.0, lastY = 0.0;
      const bool rmb =
          glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
      const bool mmb =
          glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
      const bool wasCaptured = looking || panning;
      // A drag that starts over a UI panel belongs to the UI; once a
      // viewport drag is in flight it keeps the capture until release.
      const bool mayStart = !io.WantCaptureMouse;
      looking = rmb && (wasCaptured ? looking : mayStart);
      panning = mmb && !rmb && (wasCaptured ? panning : mayStart);
      const bool nowCaptured = looking || panning;
      if (nowCaptured && !wasCaptured) {
        glfwGetCursorPos(window, &lastX, &lastY);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
      } else if (!nowCaptured && wasCaptured) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
      }

      const float groundY =
          terrain->heightAt(glm::vec2(editorCam.positionTarget().x,
                                      editorCam.positionTarget().z));
      if (nowCaptured) {
        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        const float dx = static_cast<float>(mx - lastX);
        const float dy = static_cast<float>(my - lastY);
        if (looking)
          editorCam.addLook(dx, dy);
        else // panning
          editorCam.addPan(dx, dy, groundY);
        lastX = mx;
        lastY = my;
      }

      if (!io.WantCaptureMouse && g_scrollY != 0.0)
        editorCam.addZoom(static_cast<float>(g_scrollY), groundY);
      g_scrollY = 0.0;

      // render.setParams is processed after editor-camera input in a frame,
      // while render.capture completes on a later frame. Without reseeding,
      // EditorCamera restores its old smoothed target before that capture and
      // MCP screenshots silently use the wrong viewpoint.
      static uint64_t appliedExternalCameraRevision = 0;
      if (appliedExternalCameraRevision != p.externalCameraRevision) {
        editorCam.seed(p.camPos, p.camYawDeg, p.camPitchDeg);
        appliedExternalCameraRevision = p.externalCameraRevision;
      }
      editorCam.update(dt, p.camPos, p.camYawDeg, p.camPitchDeg);
    } else {
      g_scrollY = 0.0; // don't let dolly scroll build up while playing
    }

    // --- the engine frame: gameplay -> script -> physics -> audio ----------
    // Gameplay input (player movement/aim, interactions, spaceship) only
    // reacts in play mode while running -- editor mode leaves the world
    // completely inert to keyboard/mouse gameplay input.
    VkCoreAppLayer::update(state, dt, simulate,
                           /*playerActive=*/inPlayMode && simulate);
    if (inPlayMode && state.gameplay.playerId != 0 &&
        reg.has<TransformComponent>(state.gameplay.playerId)) {
      // Player camera <- character transform. The player's yaw convention
      // is 180 degrees offset from the free-fly camera's (verified against
      // both systems' forward-vector formulas: their default rotations
      // already point the same way, at rotation.y=0 / camYawDeg=180).
      const auto &tr = reg.get<TransformComponent>(state.gameplay.playerId);
      p.camPos = tr.position;
      p.camYawDeg = tr.rotation.y + 180.0f;
      p.camPitchDeg = tr.rotation.x;

      if (std::getenv("GLGEN_SMOKE_PLAY") && (frame % 30) == 0) {
        const float groundHere =
            terrain->heightAt(glm::vec2(tr.position.x, tr.position.z));
        std::fprintf(stderr,
                     "[glGenVk] frame %d: player y=%.2f terrainY=%.2f "
                     "(delta=%.2f)\n",
                     frame, tr.position.y, groundHere,
                     tr.position.y - groundHere);
      }
    }
    audio->update(dt, p.camPos,
                  glm::normalize(glm::vec3(
                      std::cos(glm::radians(p.camPitchDeg)) *
                          std::sin(glm::radians(p.camYawDeg)),
                      std::sin(glm::radians(p.camPitchDeg)),
                      std::cos(glm::radians(p.camPitchDeg)) *
                          std::cos(glm::radians(p.camYawDeg)))));
    // Recipe hot reload: edit a *.json under assets/recipes and the mesh
    // regenerates and swaps in place under the same asset id. Polled on an
    // interval rather than every frame -- it stats one file per recipe, and
    // an authoring edit does not need sub-second latency.
    if ((frame % 30) == 0) {
      for (const std::string &m : state.assetLibrary.pollHotReload())
        LOG_INFO("Assets", m);
    }

    // Command port (AI_ASSET_PIPELINE_PLAN.md Phase 3). THE fixed point where
    // queued RPC handlers execute: on the main thread, after gameplay/physics
    // have settled this frame's state and before the renderer consumes it, so
    // an entity spawned by a handler is drawn this frame rather than next.
    agentBridge.update();

    // Editor overlay lines. Hidden in play mode and while the agent bridge is
    // capturing for review -- a grid across a screenshot is noise in both.
    debugLines.clear();
    if (!inPlayMode && !agentBridge.wantsCleanFrame()) {
      buildEditorGrid(gridSettings, renderer.params().camPos, debugLines);
      renderer.setDebugLineFadeDistance(gridSettings.fadeDistance());
    }
    renderer.setDebugLines(debugLines);

    renderSystem.update(reg, renderer);
    // Must run AFTER renderSystem.update(), which clears+rebuilds
    // mInstances from the ECS each frame -- see VkTerrainSubsystem's header
    // comment for why streamed terrain chunks aren't added through that ECS
    // path. state.terrain.streamUpdate(...) itself runs earlier, from
    // VkCoreAppLayer::update().
    terrain->addFrameInstances();

    // Per-frame input deltas are drained by consumers above; clear whatever
    // is left so stale button-changed flags don't linger across frames.
    Mouse::resetDeltas();

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    {
      // Camera matrices for the gizmo — same math as the renderer, but the
      // projection stays UNflipped (ImGuizmo maps to screen itself).
      int fbw = 0, fbh = 0;
      glfwGetFramebufferSize(window, &fbw, &fbh);
      const float aspect =
          (fbh > 0) ? (float)fbw / (float)fbh : 16.0f / 9.0f;
      const float cyaw = glm::radians(p.camYawDeg);
      const float cpitch = glm::radians(p.camPitchDeg);
      const glm::vec3 camForward = glm::normalize(
          glm::vec3(std::cos(cpitch) * std::sin(cyaw), std::sin(cpitch),
                    std::cos(cpitch) * std::cos(cyaw)));
      const glm::mat4 view = glm::lookAt(p.camPos, p.camPos + camForward,
                                         glm::vec3(0.0f, 1.0f, 0.0f));
      const glm::mat4 proj =
          glm::perspective(glm::radians(p.fovDeg), aspect, 0.05f,
                          std::max(p.farPlane, 10.0f));

      VkEditor::Context ectx{state.scene, state.assets,   state.physicsSystem,
                             renderer,    dt,             &simulate,
                             assetDir,    terrain,        &editorCam.settings,
                             &state.scriptSystem};

      // Headless verification: force play mode from frame 0 without a
      // mouse, so a smoke run can exercise the player controller (ground
      // snapping, falling-through-terrain regressions) the same way
      // clicking "> Play" does.
      static bool smokePlayRequested = false;
      if (!smokePlayRequested && std::getenv("GLGEN_SMOKE_PLAY")) {
        editor.requestPlay(ectx);
        smokePlayRequested = true;
      }

      // Same entry/exit points, driven from Lua (game.play/game.stop) so a
      // game assembled over the command port can start itself.
      if (state.requestPlayMode) {
        state.requestPlayMode = false;
        editor.requestPlay(ectx);
      }
      if (state.requestStopMode) {
        state.requestStopMode = false;
        if (editor.isInPlayMode())
          editor.stopPlayMode(ectx);
      }

      // Skip the whole editor UI on frames the agent bridge is capturing for
      // a model: the panels cover roughly a third of the frame, and a review
      // shot should show the scene, not the tool. ImGui::Render() still runs
      // so the (now empty) draw data stays valid for the overlay callback.
      if (!agentBridge.wantsCleanFrame())
        editor.draw(ectx, view, proj);

      // Session restore asked for the camera to be put on the restored
      // ground; only EditorCamera can make that stick.
      if (glm::vec3 seedPos; editor.takeCameraSeedRequest(seedPos)) {
        p.camPos = seedPos;
        editorCam.seed(p.camPos, p.camYawDeg, p.camPitchDeg);
      }

      // F: frame the selection. Handled here rather than in the editor
      // because main.cpp owns the EditorCamera; the editor only records
      // which entity was asked for.
      if (const EntityId focus = editor.takeFocusRequest()) {
        if (reg.valid(focus) && reg.has<TransformComponent>(focus)) {
          const TransformComponent &t = reg.get<TransformComponent>(focus);
          // Distance from the asset's own size so a pebble and a tree both
          // fill a comparable share of the frame.
          float radius = 1.0f;
          if (reg.has<MeshComponent>(focus)) {
            const MeshData *data =
                state.assets.getOBJData(reg.get<MeshComponent>(focus).objHandle);
            glm::vec3 mn, mx;
            if (data && data->getGlobalBounds(mn, mx)) {
              const glm::vec3 size = (mx - mn) * t.scale;
              radius = std::max({size.x, size.y, size.z, 0.1f}) * 0.5f;
            }
          }
          const float distance = std::max(radius * 3.0f, 1.5f);
          const float yaw = glm::radians(p.camYawDeg);
          const float pitch = glm::radians(p.camPitchDeg);
          // Keep the current orientation and simply back off along it, so
          // focusing does not also spin the view to some canned angle.
          const glm::vec3 forward(std::cos(pitch) * std::sin(yaw),
                                  std::sin(pitch), std::cos(pitch) * std::cos(yaw));
          const glm::vec3 target = t.position + glm::vec3(0.0f, radius * 0.5f, 0.0f);
          p.camPos = target - forward * distance;
          editorCam.seed(p.camPos, p.camYawDeg, p.camPitchDeg);
        }
      }
    }
    ImGui::Render();

    // gen-test: snapshot the original geometry, then regenerate the asset in
    // place. The swap lands at the END of the frame, so the re-upload happens
    // in the next frame's renderSystem.update() -- which is exactly the path
    // being verified.
    if (genTestEnv && capturePath && frame == genSwapFrame - 1)
      renderer.requestCapture(std::string(capturePath) + ".before.png");

    if (capturePath && maxFrames > 0 && frame == maxFrames - 1)
      renderer.requestCapture(capturePath);
    renderer.drawFrame();
    // Deferred RPC replies: the draw above is what wrote any requested PNG,
    // so this is the earliest point a capture can honestly be reported done.
    agentBridge.postFrame();

    if (genTestEnv && frame == genSwapFrame) {
      const bool ok = state.assets.replaceMeshData(
          kGenAssetId,
          makeGeneratedBox(glm::vec3(0.5f, 2.2f, 0.5f), // tall and narrow
                           glm::vec3(0.15f, 0.65f, 0.25f)));
      std::fprintf(stderr,
                   "[glGenVk] gen-test: replaceMeshData at frame %d -> %s "
                   "(contentVersion now %u)\n",
                   frame, ok ? "ok" : "FAILED",
                   state.assets.assetContentVersion(kGenAssetId));
    }
    if (maxFrames > 0 && ++frame >= maxFrames) {
      std::fprintf(stderr, "[glGenVk] Rendered %d frames; exiting.\n", frame);
      break;
    }
  }

  agentBridge.stop();
  renderer.waitIdle();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  vkDestroyDescriptorPool(ctx.device(), imguiPool, nullptr);

  state.subsystems.shutdownAll();
  renderer.shutdown();
  vkDestroySurfaceKHR(ctx.instance(), surface, nullptr);
  ctx.destroy();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

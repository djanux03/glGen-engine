// glGenVk — the real Vulkan runtime seed: a GLFW_NO_API window + Vulkan
// surface driving the engine's actual systems from EngineCore (Scene +
// AssetManager for assets, Jolt PhysicsSystem for simulation, the ECS for
// state) with VulkanRHI as the renderer. No OpenGL anywhere in the link.
//
// The demo world: static props spawned through Scene::spawnFromFile plus a
// stack of dynamic rigid bodies dropped onto a static floor — physics moves
// the TransformComponents, VulkanRenderSystem resubmits them every frame,
// and the per-frame TLAS rebuild keeps the ray-traced shadows tracking.
//
// Only built when GLGEN_BUILD_VULKAN=ON.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VulkanContext.h"
#include "VulkanRenderSystem.h"
#include "VulkanRenderer.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

// EngineCore — the engine's real, GL-free systems.
#include "Assets/AssetManager.h"
#include "Assets/MeshData.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "Scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
// Accumulated scroll, drained each frame. Installed before ImGui so ImGui's
// own callback chains to it.
double g_scrollY = 0.0;
void scrollCallback(GLFWwindow *, double, double yoffset) {
  g_scrollY += yoffset;
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
  if (!renderer.init(ctx, surface, GLGEN_VK_SHADER_DIR, queryFbSize)) {
    std::fprintf(stderr, "[glGenVk] renderer init failed\n");
    return 1;
  }

  // --- the engine: EngineCore systems, no OpenGL --------------------------
  AssetManager assets; // no GPU backend: parses CPU MeshData, Vulkan uploads
  Scene scene;
  scene.setAssetManager(&assets);
  Registry &reg = scene.registry();

  PhysicsSystem physics;
  physics.init();

  const std::string assetDir = GLGEN_VK_ASSET_DIR;
  const std::string rockPath = assetDir + "/terraingeneratorassets/rock.obj";
  const std::string treePath = assetDir + "/terraingeneratorassets/tree.obj";
  const std::string axePath = assetDir + "/playerassets/axe.obj";

  // Static floor: an invisible physics plane at the terrain's base height.
  {
    EntityId floor = scene.createEmptyEntity("Floor");
    reg.get<TransformComponent>(floor).position = glm::vec3(0.0f, -0.55f, 0.0f);
    auto &rb = reg.emplace<RigidbodyComponent>(floor);
    rb.type = RigidbodyComponent::Type::Static;
    auto &col = reg.emplace<ColliderComponent>(floor);
    col.shape = ColliderComponent::Shape::Box;
    col.dimensions = glm::vec3(80.0f, 0.1f, 80.0f);
  }

  // Static props through the real Scene::spawnFromFile path.
  {
    struct Prop {
      const std::string *path;
      glm::vec3 pos;
      float yawDeg;
      float size;
    };
    const Prop props[] = {
        {&treePath, {-1.6f, -0.5f, -1.2f}, 20.0f, 1.6f},
        {&treePath, {1.9f, -0.5f, 1.4f}, 240.0f, 1.4f},
        {&axePath, {0.8f, -0.5f, -0.9f}, 130.0f, 0.7f},
    };
    for (const Prop &p : props) {
      EntityId e = scene.spawnFromFile(*p.path);
      if (e == 0)
        continue;
      // These assets are authored off-origin; recenter so the pivot is the
      // model's base and placement is straightforward.
      assets.recenterOBJ(reg.get<MeshComponent>(e).objHandle,
                         MeshData::Recenter::BaseY);
      TransformComponent &t = reg.get<TransformComponent>(e);
      t.position = p.pos; // y = floor top; base sits on it
      t.rotation = glm::vec3(0.0f, p.yawDeg, 0.0f);
      normalizeEntityScale(reg, assets, e, p.size);
    }
  }

  // Dynamic rocks: dropped from above, simulated by the engine's Jolt
  // PhysicsSystem. Their transforms change every frame -> dynamic TLAS.
  {
    const glm::vec3 drops[] = {
        {0.0f, 1.2f, 0.0f},  {0.25f, 2.0f, 0.15f}, {-0.2f, 2.8f, -0.1f},
        {0.1f, 3.6f, 0.25f}, {-0.3f, 4.4f, 0.1f},  {0.35f, 5.2f, -0.2f},
    };
    for (const glm::vec3 &pos : drops) {
      EntityId e = scene.spawnFromFile(rockPath);
      if (e == 0)
        continue;
      // Fully centered so the mesh tumbles about the physics body's center.
      assets.recenterOBJ(reg.get<MeshComponent>(e).objHandle,
                         MeshData::Recenter::Center);
      TransformComponent &t = reg.get<TransformComponent>(e);
      t.position = pos;
      normalizeEntityScale(reg, assets, e, 0.5f);
      auto &rb = reg.emplace<RigidbodyComponent>(e);
      rb.type = RigidbodyComponent::Type::Dynamic;
      rb.mass = 2.0f;
      rb.friction = 0.7f;
      rb.restitution = 0.25f;
      auto &col = reg.emplace<ColliderComponent>(e);
      col.shape = ColliderComponent::Shape::Sphere;
      col.dimensions = glm::vec3(0.25f);
    }
  }

  vkrhi::VulkanRenderSystem renderSystem;
  renderSystem.setAssets(&assets);

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
  ImGui::StyleColorsDark();
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

  std::fprintf(stderr, "[glGenVk] Running. Close the window to exit.\n");

  int maxFrames = 0;
  if (const char *framesEnv = std::getenv("GLGEN_SMOKE_FRAMES"))
    maxFrames = std::atoi(framesEnv);
  const char *capturePath = std::getenv("GLGEN_SMOKE_CAPTURE");
  if (capturePath && maxFrames <= 0)
    maxFrames = 8;

  // A slightly raised viewpoint over the drop zone. The visual terrain is
  // kept nearly flat so it matches the flat physics floor at y = -0.5.
  {
    vkrhi::VulkanRenderer::Params &p = renderer.params();
    p.camPos = glm::vec3(0.0f, 1.1f, 3.4f);
    p.camPitchDeg = -14.0f;
    p.terrainAmplitude = 0.12f;
    p.terrainFrequency = 0.25f;
  }

  bool simulate = true;
  int frame = 0;
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    static double prevTime = glfwGetTime();
    const double nowTime = glfwGetTime();
    float dt = static_cast<float>(nowTime - prevTime);
    prevTime = nowTime;
    dt = std::min(dt, 1.0f / 30.0f); // clamp for physics stability

    ImGuiIO &io = ImGui::GetIO();
    vkrhi::VulkanRenderer::Params &p = renderer.params();

    // Mouse-look while holding the right button (cursor captured).
    static bool looking = false;
    static double lastX = 0.0, lastY = 0.0;
    const bool rmb =
        glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (rmb && !looking) {
      looking = true;
      glfwGetCursorPos(window, &lastX, &lastY);
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    } else if (!rmb && looking) {
      looking = false;
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    if (looking) {
      double mx, my;
      glfwGetCursorPos(window, &mx, &my);
      p.camYawDeg -= static_cast<float>(mx - lastX) * 0.12f;
      p.camPitchDeg -= static_cast<float>(my - lastY) * 0.12f;
      p.camPitchDeg = std::clamp(p.camPitchDeg, -89.0f, 89.0f);
      lastX = mx;
      lastY = my;
    }

    // Scroll to zoom (FOV), unless hovering the UI.
    if (!io.WantCaptureMouse && g_scrollY != 0.0)
      p.fovDeg = std::clamp(p.fovDeg - static_cast<float>(g_scrollY) * 3.0f,
                            20.0f, 90.0f);
    g_scrollY = 0.0;

    // WASD + Space/Ctrl to fly (Shift = fast), unless the UI has focus.
    if (!io.WantCaptureKeyboard) {
      const float yaw = glm::radians(p.camYawDeg);
      const float pitch = glm::radians(p.camPitchDeg);
      const glm::vec3 forward = glm::normalize(
          glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)));
      const glm::vec3 right =
          glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
      float speed = 3.0f * dt;
      if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        speed *= 3.0f;
      if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        p.camPos += forward * speed;
      if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        p.camPos -= forward * speed;
      if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        p.camPos += right * speed;
      if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        p.camPos -= right * speed;
      if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
        p.camPos.y += speed;
      if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
        p.camPos.y -= speed;
    }

    // --- the engine frame: physics -> render system -> draw ---------------
    if (simulate)
      physics.update(reg, dt);
    renderSystem.update(reg, renderer);


    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    {
      ImGui::Begin("glGen Vulkan runtime");
      ImGui::Text("EngineCore + VulkanRHI (no OpenGL)");
      ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
      ImGui::Separator();
      ImGui::TextWrapped(
          "RMB: look   Scroll: zoom   WASD + Space/Ctrl: fly (Shift = fast)");
      ImGui::Separator();
      ImGui::Checkbox("Simulate physics", &simulate);
      int meshEntities = 0;
      for (EntityId e : reg.view<MeshComponent>()) {
        (void)e;
        ++meshEntities;
      }
      ImGui::Text("Mesh entities: %d", meshEntities);
      ImGui::Separator();
      ImGui::SliderFloat("FOV", &p.fovDeg, 20.0f, 90.0f);
      ImGui::SliderFloat("Exposure", &p.exposure, 0.1f, 3.0f);
      ImGui::SliderFloat("Light yaw", &p.lightYawDeg, 0.0f, 360.0f);
      ImGui::SliderFloat("Light pitch", &p.lightPitchDeg, 5.0f, 89.0f);
      ImGui::Separator();
      ImGui::Text("Terrain generator");
      ImGui::Checkbox("Draw terrain", &p.drawTerrain);
      ImGui::SliderFloat("Amplitude", &p.terrainAmplitude, 0.0f, 3.0f);
      ImGui::SliderFloat("Frequency", &p.terrainFrequency, 0.05f, 1.5f);
      ImGui::SliderFloat("Octaves", &p.terrainOctaves, 1.0f, 8.0f, "%.0f");
      ImGui::Separator();
      ImGui::Text("Cam  %.1f, %.1f, %.1f", p.camPos.x, p.camPos.y, p.camPos.z);
      ImGui::End();
    }
    ImGui::Render();

    if (capturePath && maxFrames > 0 && frame == maxFrames - 1)
      renderer.requestCapture(capturePath);
    renderer.drawFrame();
    if (maxFrames > 0 && ++frame >= maxFrames) {
      std::fprintf(stderr, "[glGenVk] Rendered %d frames; exiting.\n", frame);
      break;
    }
  }

  renderer.waitIdle();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  vkDestroyDescriptorPool(ctx.device(), imguiPool, nullptr);

  physics.shutdown();
  renderer.shutdown();
  vkDestroySurfaceKHR(ctx.instance(), surface, nullptr);
  ctx.destroy();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

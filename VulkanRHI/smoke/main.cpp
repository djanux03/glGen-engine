// Vulkan smoke test / interactive viewer: GLFW window + Vulkan surface driving
// the VulkanRenderer (shadow -> HDR scene -> tonemap), with a Dear ImGui control
// panel. ImGui lives entirely here; the renderer only exposes an overlay
// callback (recorded inside the tonemap pass) and a live Params struct.
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

// The engine's real ECS (now OpenGL-free) drives the scene.
#include "ECS/Components.h"
#include "ECS/Registry.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// Accumulated scroll, drained each frame. Installed before ImGui so ImGui's
// own callback chains to it.
double g_scrollY = 0.0;
void scrollCallback(GLFWwindow *, double, double yoffset) {
  g_scrollY += yoffset;
}
} // namespace

#ifndef GLGEN_VK_SHADER_DIR
#define GLGEN_VK_SHADER_DIR "."
#endif
#ifndef GLGEN_VK_MODEL_PATH
#define GLGEN_VK_MODEL_PATH "model.obj"
#endif

int main() {
  if (!glfwInit()) {
    std::fprintf(stderr, "[smoke] glfwInit failed\n");
    return 1;
  }
  if (!glfwVulkanSupported()) {
    std::fprintf(stderr, "[smoke] GLFW reports Vulkan not supported\n");
    glfwTerminate();
    return 1;
  }

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window =
      glfwCreateWindow(1280, 720, "glGen Vulkan", nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "[smoke] glfwCreateWindow failed\n");
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
    std::fprintf(stderr, "[smoke] createInstance failed\n");
    return 1;
  }

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (glfwCreateWindowSurface(ctx.instance(), window, nullptr, &surface) !=
      VK_SUCCESS) {
    std::fprintf(stderr, "[smoke] glfwCreateWindowSurface failed\n");
    return 1;
  }

  if (!ctx.selectAndCreateDevice(surface)) {
    std::fprintf(stderr, "[smoke] device selection failed\n");
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
    std::fprintf(stderr, "[smoke] renderer init failed\n");
    return 1;
  }

  // Build the scene in the engine's ECS. VulkanRenderSystem walks it every
  // frame (below) — transforms are live, and the TLAS is rebuilt per frame.
  Registry registry;
  {
    auto spawn = [&](const std::string &path, glm::vec3 pos, float yawDeg,
                     float scale) {
      EntityId e = registry.create();
      TransformComponent &t = registry.emplace<TransformComponent>(e);
      t.position = pos;
      t.rotation = glm::vec3(0.0f, yawDeg, 0.0f);
      t.scale = glm::vec3(scale);
      MeshComponent &mc = registry.emplace<MeshComponent>(e);
      mc.assetId = path; // resolved to a renderer mesh by VulkanRenderSystem
    };

    std::vector<std::string> paths = {GLGEN_VK_MODEL_PATH};
#ifdef GLGEN_VK_MODEL_PATH2
    paths.push_back(GLGEN_VK_MODEL_PATH2);
#endif
#ifdef GLGEN_VK_MODEL_PATH3
    paths.push_back(GLGEN_VK_MODEL_PATH3);
#endif
    struct Slot {
      float x, z, yawDeg, scale;
    };
    const Slot slots[] = {
        {0.0f, 0.0f, 25.0f, 0.50f},    {-1.3f, 0.6f, 200.0f, 0.40f},
        {1.2f, -0.8f, 120.0f, 0.45f},  {1.1f, 1.1f, 70.0f, 0.42f},
        {-1.0f, -1.1f, 310.0f, 0.44f}, {0.4f, 1.4f, 160.0f, 0.38f},
    };
    size_t k = 0;
    for (const Slot &s : slots) {
      spawn(paths[k % paths.size()],
            glm::vec3(s.x, -0.5f + 0.5f * s.scale, s.z), s.yawDeg, s.scale);
      ++k;
    }
  }
  vkrhi::VulkanRenderSystem renderSystem;

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

  std::fprintf(stderr, "[smoke] Running. Close the window to exit.\n");

  int maxFrames = 0;
  if (const char *framesEnv = std::getenv("GLGEN_SMOKE_FRAMES"))
    maxFrames = std::atoi(framesEnv);
  const char *capturePath = std::getenv("GLGEN_SMOKE_CAPTURE");
  if (capturePath && maxFrames <= 0)
    maxFrames = 8;

  int frame = 0;
  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // --- free-fly camera input ---
    static double prevTime = glfwGetTime();
    const double nowTime = glfwGetTime();
    const float dt = static_cast<float>(nowTime - prevTime);
    prevTime = nowTime;

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

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    {
      ImGui::Begin("glGen Vulkan");
      ImGui::Text("RTX 3070 - RT shadows + mesh-shader terrain");
      ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
      ImGui::Separator();
      ImGui::TextWrapped(
          "RMB: look   Scroll: zoom   WASD + Space/Ctrl: fly (Shift = fast)");
      ImGui::Separator();
      
      if (ImGui::CollapsingHeader("Camera & Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("FOV", &p.fovDeg, 20.0f, 90.0f);
        ImGui::Text("Cam  %.1f, %.1f, %.1f", p.camPos.x, p.camPos.y, p.camPos.z);
      }

      if (ImGui::CollapsingHeader("Lighting & Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Light yaw", &p.lightYawDeg, 0.0f, 360.0f);
        ImGui::SliderFloat("Light pitch", &p.lightPitchDeg, 5.0f, 89.0f);
        ImGui::SliderFloat("Ambient Intensity", &p.ambientIntensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Shadow Strength", &p.shadowStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Shadow Softness", &p.shadowSoftness, 0.0f, 5.0f);
        ImGui::SliderInt("Shadow Samples", &p.shadowSamples, 1, 16);
      }

      if (ImGui::CollapsingHeader("Atmosphere & Sky")) {
        ImGui::SliderFloat("Haze (Mie)", &p.atmosphereHaze, 0.0f, 1.0f);
        ImGui::SliderFloat("Sky Brightness", &p.skyBrightness, 0.0f, 3.0f);
        ImGui::SliderFloat("Sun Disc Intensity", &p.sunDiscIntensity, 0.0f, 100.0f);
        ImGui::SliderFloat("Stars", &p.starIntensity, 0.0f, 3.0f);
        ImGui::SliderFloat("Moonlight", &p.moonIntensity, 0.0f, 4.0f);
        ImGui::SliderFloat("Night Sky Glow", &p.nightSkyBrightness, 0.0f, 3.0f);
      }

      if (ImGui::CollapsingHeader("Volumetrics & Fog")) {
        ImGui::SliderFloat("Fog Density", &p.fogDensity, 0.0f, 0.2f, "%.4f");
        ImGui::SliderFloat("Fog Start", &p.fogStart, 0.0f, 100.0f);
        ImGui::SliderFloat("Fog Max Opacity", &p.fogMaxOpacity, 0.0f, 1.0f);
        ImGui::SliderFloat("Fog Height Falloff", &p.fogHeightFalloff, 0.0f, 2.0f);
      }

      if (ImGui::CollapsingHeader("Post-Processing & Tonemapping", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Exposure", &p.exposure, 0.1f, 5.0f);
        const char* tonemapModes[] = { "Painterly", "ACES", "Reinhard", "Linear" };
        ImGui::Combo("Tonemap Mode", &p.tonemapMode, tonemapModes, IM_ARRAYSIZE(tonemapModes));
        ImGui::SliderFloat("Gamma", &p.gamma, 1.0f, 3.0f);
        ImGui::SliderFloat("Saturation", &p.saturation, 0.0f, 2.0f);
        ImGui::SliderFloat("Contrast", &p.contrast, 0.0f, 2.0f);
        ImGui::SliderFloat("Vignette", &p.vignette, 0.0f, 1.0f);
      }

      // Terrain is now a CPU-built chunk mesh owned by glGenVk's
      // VkTerrainSubsystem (TERRAIN_GENERATOR_PLAN.md); this standalone
      // smoke app has no terrain of its own to configure anymore.
      if (ImGui::CollapsingHeader("Terrain Materials")) {
        ImGui::ColorEdit3("Sand Color", &p.terrainColorSand.x);
        ImGui::ColorEdit3("Grass Color", &p.terrainColorGrass.x);
        ImGui::ColorEdit3("Rock Color", &p.terrainColorRock.x);
        ImGui::ColorEdit3("Snow Color", &p.terrainColorSnow.x);
        ImGui::Separator();
        ImGui::SliderFloat("Grass Start", &p.grassStart, -1.0f, 0.0f);
        ImGui::SliderFloat("Grass End", &p.grassEnd, -0.5f, 0.5f);
        ImGui::SliderFloat("Snow Start", &p.snowStart, 0.0f, 1.0f);
        ImGui::SliderFloat("Snow End", &p.snowEnd, 0.5f, 1.5f);
        ImGui::SliderFloat("Rock Slope Start", &p.rockSlopeStart, 0.0f, 1.0f);
        ImGui::SliderFloat("Rock Slope End", &p.rockSlopeEnd, 0.0f, 1.0f);
        ImGui::SliderFloat("Detail Scale", &p.terrainDetailScale, 0.01f, 2.0f);
        ImGui::SliderFloat("Detail Strength", &p.terrainDetailStrength, 0.0f, 1.0f);
      }

      ImGui::End();
    }
    ImGui::Render();

    // Animate the ECS: slow spin on every mesh entity. This exercises the
    // dynamic path — VulkanRenderSystem resubmits transforms and drawFrame
    // rebuilds the frame's TLAS, so the RT shadows track the motion.
    for (EntityId e : registry.view<MeshComponent>()) {
      if (!registry.has<TransformComponent>(e))
        continue;
      TransformComponent &t = registry.get<TransformComponent>(e);
      t.rotation.y += 45.0f * dt;
      if (t.rotation.y > 360.0f)
        t.rotation.y -= 360.0f;
    }
    renderSystem.update(registry, renderer);

    if (capturePath && maxFrames > 0 && frame == maxFrames - 1)
      renderer.requestCapture(capturePath);
    renderer.drawFrame();
    if (maxFrames > 0 && ++frame >= maxFrames) {
      std::fprintf(stderr, "[smoke] Rendered %d frames; exiting.\n", frame);
      break;
    }
  }

  renderer.waitIdle();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  vkDestroyDescriptorPool(ctx.device(), imguiPool, nullptr);

  renderer.shutdown();
  vkDestroySurfaceKHR(ctx.instance(), surface, nullptr);
  ctx.destroy();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

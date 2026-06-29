// Vulkan smoke test / interactive viewer: GLFW window + Vulkan surface driving
// the VulkanRenderer (shadow -> HDR scene -> tonemap), with a Dear ImGui control
// panel. ImGui lives entirely here; the renderer only exposes an overlay
// callback (recorded inside the tonemap pass) and a live Params struct.
//
// Only built when GLGEN_BUILD_VULKAN=ON.
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VulkanContext.h"
#include "VulkanRenderer.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

#include <cstdio>
#include <vector>

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
  if (!renderer.init(ctx, surface, GLGEN_VK_SHADER_DIR, GLGEN_VK_MODEL_PATH,
                     queryFbSize)) {
    std::fprintf(stderr, "[smoke] renderer init failed\n");
    return 1;
  }

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

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    {
      vkrhi::VulkanRenderer::Params &p = renderer.params();
      ImGui::Begin("glGen Vulkan");
      ImGui::Text("RTX 3070 - mesh-shader terrain + CSM");
      ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                  1000.0f / ImGui::GetIO().Framerate);
      ImGui::Separator();
      ImGui::SliderFloat("Exposure", &p.exposure, 0.1f, 3.0f);
      ImGui::SliderFloat("Light yaw", &p.lightYawDeg, 0.0f, 360.0f);
      ImGui::SliderFloat("Light pitch", &p.lightPitchDeg, 5.0f, 89.0f);
      ImGui::Separator();
      ImGui::Checkbox("Auto-orbit camera", &p.autoOrbit);
      ImGui::SliderFloat("Cam yaw", &p.camYawDeg, -180.0f, 180.0f);
      ImGui::SliderFloat("Cam pitch", &p.camPitchDeg, -10.0f, 80.0f);
      ImGui::SliderFloat("Cam distance", &p.camDistance, 1.2f, 6.0f);
      ImGui::Checkbox("Draw terrain", &p.drawTerrain);
      ImGui::End();
    }
    ImGui::Render();

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

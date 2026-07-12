#pragma once

#include "VulkanRenderer.h"

#include <string>
#include <unordered_map>

class Registry; // engine ECS (header-only, GL-free)

namespace vkrhi {

// The Vulkan counterpart of the GL RenderSystem: walks the engine's ECS once
// per frame and feeds the renderer. MeshComponent.assetId paths resolve to
// renderer meshes (cached); every visible entity submits an instance with its
// current world transform, and drawFrame() rebuilds the frame's TLAS from
// them — so transforms may change freely between frames.
class VulkanRenderSystem {
public:
  // Call once per frame, before renderer.drawFrame(). Returns false when the
  // scene could not be finalized (no loadable meshes yet).
  bool update(Registry &registry, VulkanRenderer &renderer);

private:
  // assetId -> renderer mesh; failures cached as UINT32_MAX so a missing
  // file is only reported once.
  std::unordered_map<std::string, VulkanRenderer::MeshHandle> mMeshByAsset;
};

} // namespace vkrhi

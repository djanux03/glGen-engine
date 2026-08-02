#pragma once

#include "VulkanRenderer.h"

#include <string>
#include <unordered_map>

class Registry;     // engine ECS (header-only, GL-free)
class AssetManager; // engine asset manager (EngineCore, GL-free)

namespace vkrhi {

// The Vulkan counterpart of the GL RenderSystem: walks the engine's ECS once
// per frame and feeds the renderer. MeshComponent assets resolve to renderer
// meshes (cached); every visible entity submits an instance with its current
// world transform, and drawFrame() rebuilds the frame's TLAS from them — so
// transforms may change freely between frames.
//
// With an AssetManager attached, meshes come from the engine's parsed CPU
// MeshData (any format the engine parses, authored scale, primitives).
// Without one, assetId is treated as an OBJ path (legacy smoke-test path).
class VulkanRenderSystem {
public:
  void setAssets(AssetManager *assets) { mAssets = assets; }

  // Call once per frame, before renderer.drawFrame(). Returns false when the
  // scene could not be finalized (no loadable meshes yet).
  bool update(Registry &registry, VulkanRenderer &renderer);

private:
  // A resolved asset: its renderer mesh plus the AssetManager content version
  // that mesh was built from. When the two diverge (a generated asset was
  // regenerated, or recenterOBJ/rotateOBJ mutated the CPU mesh), the geometry
  // is re-uploaded in place via VulkanRenderer::updateMeshFromData() -- the
  // handle stays stable, so nothing else has to be told.
  struct CachedMesh {
    VulkanRenderer::MeshHandle handle = UINT32_MAX;
    uint32_t contentVersion = 0;
  };

  AssetManager *mAssets = nullptr;
  // assetId -> renderer mesh; failures cached with handle == UINT32_MAX so a
  // missing file is only reported once.
  std::unordered_map<std::string, CachedMesh> mMeshByAsset;
};

} // namespace vkrhi

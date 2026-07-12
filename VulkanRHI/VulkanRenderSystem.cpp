#include "VulkanRenderSystem.h"

#include "Assets/AssetManager.h"
#include "Assets/MeshData.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace vkrhi {

namespace {

// World matrix with HierarchyComponent parent chains (same rule as the GL
// RenderSystem: parentWorld * local).
glm::mat4 worldMatrix(Registry &registry, EntityId e, int depth = 0) {
  glm::mat4 local = registry.get<TransformComponent>(e).getMatrix();
  if (depth < 32 && registry.has<HierarchyComponent>(e)) {
    const auto &h = registry.get<HierarchyComponent>(e);
    if (h.parent != 0 && registry.has<TransformComponent>(h.parent))
      return worldMatrix(registry, h.parent, depth + 1) * local;
  }
  return local;
}

bool entityAlive(Registry &registry, EntityId e) {
  if (!registry.has<LifecycleComponent>(e))
    return true;
  return registry.get<LifecycleComponent>(e).state ==
         EntityLifecycleState::Alive;
}

// Resolves an entity's mesh to the engine's parsed CPU data, loading it
// through the AssetManager if the component doesn't carry a handle yet
// (also covers "__primitive_*" ids).
const ::MeshData *resolveMeshData(AssetManager &assets, MeshComponent &mc) {
  if (mc.objHandle.valid())
    if (const ::MeshData *d = assets.getOBJData(mc.objHandle))
      return d;
  if (mc.gltfHandle.valid())
    if (const ::MeshData *d = assets.getGLTFData(mc.gltfHandle))
      return d;
  if (mc.ufbxHandle.valid())
    if (const ::MeshData *d = assets.getUFBXData(mc.ufbxHandle))
      return d;

  std::string ext =
      std::filesystem::path(mc.assetId).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });

  if (ext == ".obj" || mc.assetId.rfind("__primitive_", 0) == 0) {
    mc.objHandle = assets.loadOBJ(mc.assetId);
    return assets.getOBJData(mc.objHandle);
  }
  if (ext == ".gltf" || ext == ".glb") {
    mc.gltfHandle = assets.loadGLTF(mc.assetId);
    return assets.getGLTFData(mc.gltfHandle);
  }
  if (ext == ".fbx") {
    mc.ufbxHandle = assets.loadUFBX(mc.assetId);
    return assets.getUFBXData(mc.ufbxHandle);
  }
  return nullptr;
}

} // namespace

bool VulkanRenderSystem::update(Registry &registry, VulkanRenderer &renderer) {
  renderer.clearInstances();

  bool meshesAdded = false;
  for (EntityId e : registry.view<MeshComponent>()) {
    if (!registry.has<TransformComponent>(e))
      continue;
    if (!entityAlive(registry, e))
      continue;
    MeshComponent &mc = registry.get<MeshComponent>(e);
    if (!mc.visible || mc.assetId.empty())
      continue;

    VulkanRenderer::MeshHandle handle;
    auto it = mMeshByAsset.find(mc.assetId);
    if (it != mMeshByAsset.end()) {
      handle = it->second;
    } else {
      if (mAssets) {
        const ::MeshData *data = resolveMeshData(*mAssets, mc);
        handle = data ? renderer.createMeshFromData(*data, mc.assetId)
                      : UINT32_MAX;
      } else {
        handle = renderer.createMeshFromObj(mc.assetId);
      }
      mMeshByAsset[mc.assetId] = handle;
      if (handle != UINT32_MAX)
        meshesAdded = true;
    }
    if (handle == UINT32_MAX)
      continue;

    renderer.addInstance(handle, worldMatrix(registry, e));
  }

  // First scene, or new meshes since the last finalize: (re)build the BLAS
  // set and per-frame TLAS resources.
  if (meshesAdded || !renderer.sceneReady()) {
    if (!renderer.finalizeScene())
      return false;
  }
  return true;
}

} // namespace vkrhi

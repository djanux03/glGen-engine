#include "VulkanRenderSystem.h"

#include "ECS/Components.h"
#include "ECS/Registry.h"

#include <glm/glm.hpp>

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
      handle = renderer.createMeshFromObj(mc.assetId);
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

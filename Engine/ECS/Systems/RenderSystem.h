#pragma once
#include "Assets/FBXModel.h"
#include "Assets/OBJModel.h"
#include "Assets/UFBXModel.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Rendering/Shader.h"
#include <algorithm>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_map>

class RenderSystem {
public:
  struct VisibilityStats {
    int tested = 0;
    int drawn = 0;
    int culled = 0;
    int drawCallsMain = 0;
    int drawCallsShadow = 0;
    int instancedDrawCallsMain = 0;
    int instancedDrawCallsShadow = 0;
  };

  void beginFrame() { mStats = {}; }

  void setViewProjection(const glm::mat4 &vp) {
    mViewProjection = vp;
    // Pre-compute frustum planes once (6 dot products per entity vs full
    // extraction)
    const glm::vec4 r0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
    const glm::vec4 r1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
    const glm::vec4 r2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
    const glm::vec4 r3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

    glm::vec4 raw[6] = {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2};
    for (int i = 0; i < 6; ++i) {
      float len = glm::length(glm::vec3(raw[i]));
      mFrustumPlanes[i] = (len > 1e-5f) ? raw[i] / len : raw[i];
    }
  }
  void setCameraPosition(const glm::vec3 &p) { mCameraPos = p; }
  void setCullingEnabled(bool enabled) { mCullingEnabled = enabled; }
  bool cullingEnabled() const { return mCullingEnabled; }
  const VisibilityStats &stats() const { return mStats; }

  void update(Registry &registry, Shader &shader, bool shadowPass = false,
              EntityId selectedEntity = 0, bool outlinePass = false) {

    // Set pass-level uniforms ONCE (instead of per-object in OBJModel::draw)
    if (!shadowPass) {
      shader.setBool("uGlowPass", false);
      shader.setBool("uCloudPass", false);
      shader.setInt("texture1", 0);
    }

    // Reuse allocations across frames
    mWorldCache.clear();
    mVisit.clear();
    mDrawList.clear();

    auto worldMatrix = [&](auto &&self, EntityId e) -> glm::mat4 {
      auto itV = mVisit.find(e);
      if (itV != mVisit.end() && itV->second == 2)
        return mWorldCache[e];
      if (itV != mVisit.end() && itV->second == 1)
        return registry.get<TransformComponent>(e).getMatrix();

      mVisit[e] = 1;
      glm::mat4 local = registry.get<TransformComponent>(e).getMatrix();
      glm::mat4 world = local;

      if (registry.has<HierarchyComponent>(e)) {
        auto &h = registry.get<HierarchyComponent>(e);
        if (h.parent != 0 && registry.has<TransformComponent>(h.parent)) {
          world = self(self, h.parent) * local;
        }
      }

      mVisit[e] = 2;
      mWorldCache[e] = world;
      return world;
    };

    // ------------------------------------------------------------------
    // Draw call sorting: collect visible entities, sort by model pointer
    // to batch same-model draws (reduces VAO/texture rebinds).
    // ------------------------------------------------------------------

    for (auto entity :
         registry.viewWhere<MeshComponent, TransformComponent>([&](EntityId e) {
           if (!registry.has<LifecycleComponent>(e))
             return true;
           auto s = registry.get<LifecycleComponent>(e).state;
           return s == EntityLifecycleState::Alive;
         })) {
      auto &mesh = registry.get<MeshComponent>(entity);

      if (!mesh.visible)
        continue;
      if (shadowPass && !mesh.castsShadow)
        continue;
      if (!mesh.objModel && !mesh.gltfModel && !mesh.ufbxModel)
        continue;
      if (outlinePass && entity != selectedEntity)
        continue;

      // Frustum culling (only main pass, skip terrain)
      if (!shadowPass && mCullingEnabled && !mesh.isTerrain) {
        ++mStats.tested;
        glm::mat4 world = worldMatrix(worldMatrix, entity);
        float radius = 1.0f;
        if (registry.has<BoundsComponent>(entity))
          radius = registry.get<BoundsComponent>(entity).radius;
        const glm::vec3 center = glm::vec3(world[3]);

        if (registry.has<LODComponent>(entity)) {
          const auto &lod = registry.get<LODComponent>(entity);
          const float d = glm::length(mCameraPos - center);
          if (d < lod.minDistance || d > lod.maxDistance) {
            ++mStats.culled;
            continue;
          }
        }
        if (!sphereInFrustum_(center, radius)) {
          ++mStats.culled;
          continue;
        }
      }

      uint64_t modelKey = 0;
      if (mesh.objModel)
        modelKey = (uint64_t)reinterpret_cast<uintptr_t>(mesh.objModel);
      else if (mesh.gltfModel)
        modelKey = (uint64_t)reinterpret_cast<uintptr_t>(mesh.gltfModel);
      else if (mesh.ufbxModel)
        modelKey = (uint64_t)reinterpret_cast<uintptr_t>(mesh.ufbxModel);

      uint64_t materialKey = 0;
      if (registry.has<MaterialOverrideComponent>(entity)) {
        auto &mo = registry.get<MaterialOverrideComponent>(entity);
        if (mo.enabled) {
          // Prefer stable string id when present; fallback to texture ids.
          if (!mo.material.id.empty()) {
            materialKey = (uint64_t)std::hash<std::string>{}(mo.material.id);
          } else {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](uint64_t v) {
              h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            };
            mix((uint64_t)mo.material.texDiffuse);
            mix((uint64_t)mo.material.texNormal);
            mix((uint64_t)mo.material.texRoughness);
            mix((uint64_t)mo.material.texMetallic);
            mix((uint64_t)mo.material.texAO);
            mix((uint64_t)mo.material.texEmissive);
            mix((uint64_t)mo.material.texOpacity);
            materialKey = h;
          }
        }
      }

      // Combine material + model keys to group similar material overrides.
      uint64_t key = materialKey;
      key ^= modelKey + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
      mDrawList.push_back({entity, key});
    }

    std::sort(mDrawList.begin(), mDrawList.end(),
              [](const DrawItem &a, const DrawItem &b) {
                return a.sortKey < b.sortKey;
              });

    // ------------------------------------------------------------------
    // Execute sorted draw calls — pass world matrix directly, no decompose
    // ------------------------------------------------------------------
    for (const auto &item : mDrawList) {
      auto &mesh = registry.get<MeshComponent>(item.entity);
      glm::mat4 world = worldMatrix(worldMatrix, item.entity);

      // Extract TRS from matrix directly (much faster than glm::decompose)
      glm::vec3 pos(world[3]);
      glm::vec3 scale(glm::length(glm::vec3(world[0])),
                      glm::length(glm::vec3(world[1])),
                      glm::length(glm::vec3(world[2])));
      glm::mat4 rotMat = world;
      if (scale.x > 1e-6f)
        rotMat[0] /= scale.x;
      if (scale.y > 1e-6f)
        rotMat[1] /= scale.y;
      if (scale.z > 1e-6f)
        rotMat[2] /= scale.z;
      glm::quat rotQ = glm::quat_cast(rotMat);
      glm::vec3 rot = glm::degrees(glm::eulerAngles(rotQ));

      const int submeshCount = [&]() -> int {
        if (mesh.objModel)
          return (int)mesh.objModel->submeshCount();
        if (mesh.gltfModel)
          return (int)mesh.gltfModel->submeshCount();
        if (mesh.ufbxModel)
          return (int)mesh.ufbxModel->submeshCount();
        return 0;
      }();

      if (shadowPass) {
        mStats.drawCallsShadow += submeshCount;
        if (mesh.objModel) {
          mesh.objModel->drawDepth(shader, pos, rot, scale);
        } else if (mesh.gltfModel) {
          mesh.gltfModel->drawDepth(shader, pos, rot, scale);
        } else if (mesh.ufbxModel) {
          mesh.ufbxModel->drawDepth(shader, pos, rot, scale);
        }
      } else {
        mStats.drawCallsMain += submeshCount;
        // Stencil writing logic for selected entity
        if (!outlinePass && selectedEntity != 0) {
          if (item.entity == selectedEntity) {
            glStencilFunc(GL_ALWAYS, 1, 0xFF);
            glStencilMask(0xFF);
          } else {
            glStencilMask(0x00);
          }
        }

        if (mesh.isTerrain) {
          shader.setBool("uTerrainPass", true);
          shader.setBool("uUseColor", false);
        }

        const MaterialAsset *materialOverride = nullptr;
        if (registry.has<MaterialOverrideComponent>(item.entity)) {
          auto &mo = registry.get<MaterialOverrideComponent>(item.entity);
          if (mo.enabled) {
            materialOverride = &mo.material;
          }
        }

        if (mesh.objModel) {
          mesh.objModel->draw(shader, pos, rot, scale, materialOverride);
          ++mStats.drawn;
        } else if (mesh.gltfModel) {
          mesh.gltfModel->draw(shader, pos, rot, scale, materialOverride);
          ++mStats.drawn;
        } else if (mesh.ufbxModel) {
          mesh.ufbxModel->draw(shader, pos, rot, scale, materialOverride);
          ++mStats.drawn;
        }

        if (mesh.isTerrain) {
          shader.setBool("uTerrainPass", false);
        }
      }
    }

    // ------------------------------------------------------------------
    // Draw Instanced Meshes
    // ------------------------------------------------------------------
    for (auto entity : registry.view<InstancedMeshComponent>()) {
      if (!registry.has<LifecycleComponent>(entity))
        continue;
      if (registry.get<LifecycleComponent>(entity).state !=
          EntityLifecycleState::Alive)
        continue;

      auto &inst = registry.get<InstancedMeshComponent>(entity);
      if (!inst.visible || (!inst.objModel && !inst.ufbxModel))
        continue;
      if (shadowPass && !inst.castsShadow)
        continue;
      if (inst.instanceTransforms.empty())
        continue;

      // ------------------------------------------------------------------
      // Per-instance CPU culling (Unreal HISM / Unity MultiMesh style)
      // Instead of uploading all instances, filter to only those visible:
      //   1. Within maxDrawDistance of the camera
      //   2. Inside the view frustum (or shadow frustum)
      // We build a temporary scratch list and upload only the survivors.
      // The authoritative instanceTransforms list is never mutated here.
      // ------------------------------------------------------------------
      const float maxDist = inst.maxDrawDistance;
      const float maxDist2 = maxDist * maxDist;
      // Approximate bounding radius of one tree instance (half the scale on Y)
      constexpr float kTreeRadius = 8.0f;

      // Build culled list — only allocate if count would change
      const int totalCount = (int)inst.instanceTransforms.size();
      mCullScratch.clear();
      mCullScratch.reserve(totalCount);

      for (const glm::mat4 &m : inst.instanceTransforms) {
        const glm::vec3 worldPos(m[3]);
        // Distance cull (squared, no sqrt)
        glm::vec3 d = worldPos - mCameraPos;
        if (d.x * d.x + d.y * d.y + d.z * d.z > maxDist2)
          continue;
        // Frustum cull — test if sphere around instance position is visible
        // Shadow pass uses a slightly looser check to avoid shadow popping
        bool visible = true;
        const float r = shadowPass ? kTreeRadius * 2.0f : kTreeRadius;
        for (const glm::vec4 &p : mFrustumPlanes) {
          if (glm::dot(glm::vec3(p), worldPos) + p.w < -r) {
            visible = false;
            break;
          }
        }
        if (visible)
          mCullScratch.push_back(m);
      }

      const int visibleCount = (int)mCullScratch.size();
      if (visibleCount == 0) {
        shader.setBool("uInstanced", false);
        continue;
      }

      // Upload culled transforms to GPU (orphan when capacity exceeded)
      if (inst.instanceVBO == 0)
        glGenBuffers(1, &inst.instanceVBO);

      const size_t neededBytes = (size_t)visibleCount * sizeof(glm::mat4);
      glBindBuffer(GL_ARRAY_BUFFER, inst.instanceVBO);
      if (neededBytes > inst.instanceVBOCapacity) {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)neededBytes, nullptr,
                     GL_DYNAMIC_DRAW);
        inst.instanceVBOCapacity = neededBytes;
      }
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)neededBytes,
                      mCullScratch.data());
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      // isDirty is no longer needed with per-frame culled upload
      inst.isDirty = false;

      shader.setBool("uInstanced", true);

      const int instSubmeshCount = [&]() -> int {
        if (inst.objModel)
          return (int)inst.objModel->submeshCount();
        if (inst.ufbxModel)
          return (int)inst.ufbxModel->submeshCount();
        return 0;
      }();

      if (shadowPass) {
        mStats.instancedDrawCallsShadow += instSubmeshCount;
        if (inst.objModel) {
          inst.objModel->drawDepthInstanced(shader, inst.instanceVBO,
                                            visibleCount);
        } else if (inst.ufbxModel) {
          inst.ufbxModel->drawDepthInstanced(shader, inst.instanceVBO,
                                             visibleCount);
        }
      } else {
        mStats.instancedDrawCallsMain += instSubmeshCount;
        shader.setBool("uTerrainPass", inst.useTerrainShading);
        shader.setBool("uUseColor", false);

        if (inst.objModel) {
          inst.objModel->drawInstanced(shader, inst.instanceVBO, visibleCount);
        } else if (inst.ufbxModel) {
          inst.ufbxModel->drawInstanced(shader, inst.instanceVBO, visibleCount);
        }
        mStats.drawn += visibleCount;
        mStats.culled += (totalCount - visibleCount);

        shader.setBool("uTerrainPass", false);
      }

      shader.setBool("uInstanced", false);
    }
  }

private:
  // Culls a sphere against pre-computed (and pre-normalized) frustum planes
  bool sphereInFrustum_(const glm::vec3 &center, float radius) const {
    for (const glm::vec4 &p : mFrustumPlanes) {
      if (glm::dot(glm::vec3(p), center) + p.w < -radius)
        return false;
    }
    return true;
  }

  // DrawItem is defined at class scope so mDrawList can be a member
  struct DrawItem {
    EntityId entity;
    uint64_t sortKey;
  };

  glm::mat4 mViewProjection{1.0f};
  glm::vec3 mCameraPos{0.0f};
  glm::vec4 mFrustumPlanes[6]{};
  bool mCullingEnabled = true;
  VisibilityStats mStats{};

  // Persistent per-frame caches — cleared each frame, capacity retained
  std::unordered_map<EntityId, glm::mat4> mWorldCache;
  std::unordered_map<EntityId, uint8_t> mVisit;
  std::vector<DrawItem> mDrawList;
  // Scratch buffer for per-instance frustum culling (avoids malloc each frame)
  std::vector<glm::mat4> mCullScratch;
};

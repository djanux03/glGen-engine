#pragma once
#include "Assets/FBXModel.h"
#include "Assets/OBJModel.h"
#include "Assets/UFBXModel.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Rendering/RenderCapabilities.h"
#include "Rendering/Shader.h"
#include "Rendering/Texture.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>
#include <limits>
#include <unordered_map>
#include <vector>

class RenderSystem {
public:
  enum class TerrainFilter { All, OnlyTerrain, ExcludeTerrain };

  struct VisibilityStats {
    int tested = 0;
    int drawn = 0;
    int culled = 0;
    int drawCallsMain = 0;
    int drawCallsShadow = 0;
    int instancedDrawCallsMain = 0;
    int instancedDrawCallsShadow = 0;
    int instancedUploadsMain = 0;
    int instancedUploadsShadow = 0;
    int instancedUploadSkipsMain = 0;
    int instancedUploadSkipsShadow = 0;
    int instancedUploadBytesMain = 0;
    int instancedUploadBytesShadow = 0;
    int instancedClustersTestedMain = 0;
    int instancedClustersTestedShadow = 0;
    int instancedClustersVisibleMain = 0;
    int instancedClustersVisibleShadow = 0;
    int shadowDistanceCulled = 0;
    int shadowSmallCasterCulled = 0;
  };

  void beginFrame() {
    mStats = {};
    mUploadBytesUsed = 0;
    mFramePacketsPrepared = false;
    mFramePackets.clear();
    mEntityPacketIndex.clear();
  }

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
  void setShadowCameraCulling(bool enabled) {
    mShadowCameraCulling = enabled;
  }
  void setSubmissionBackend(RenderSubmissionBackend backend) {
    mSubmissionBackend = backend;
  }
  void setFrameUploadBudgetBytes(size_t bytes) { mUploadBudgetBytes = bytes; }
  void setShadowDistanceLimit(float limit) {
    mShadowDistanceLimit = (limit > 0.0f)
                               ? limit
                               : std::numeric_limits<float>::infinity();
  }
  bool cullingEnabled() const { return mCullingEnabled; }
  const VisibilityStats &stats() const { return mStats; }
  void prepareFrame(Registry &registry) { prepareFramePackets_(registry); }

  void update(Registry &registry, Shader &shader, bool shadowPass = false,
              EntityId selectedEntity = 0, bool outlinePass = false,
              bool viewModelPass = false,
              TerrainFilter terrainFilter = TerrainFilter::All) {

    // Set pass-level uniforms ONCE (instead of per-object in OBJModel::draw)
    if (!shadowPass) {
      shader.setBool("uGlowPass", false);
      shader.setBool("uCloudPass", false);
      shader.setInt("texture1", 0);
    }

    if (!updateMeshPackets_(registry, shader, shadowPass, selectedEntity,
                            outlinePass, viewModelPass, terrainFilter)) {

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
      if (viewModelPass) {
        if (!mesh.isViewModel)
          continue;
      } else {
        if (mesh.isViewModel)
          continue;
      }
      if (shadowPass && !mesh.castsShadow)
        continue;
      if (!mesh.objModel && !mesh.gltfModel && !mesh.ufbxModel)
        continue;
      if (outlinePass && entity != selectedEntity)
        continue;
      if (terrainFilter == TerrainFilter::OnlyTerrain &&
          (!mesh.isTerrain || mesh.isWater))
        continue;
      if (terrainFilter == TerrainFilter::ExcludeTerrain &&
          (mesh.isTerrain && !mesh.isWater))
        continue;

      // Frustum culling (main pass + optional shadow camera culling).
      // Terrain chunks provide an explicit bounds center offset because their
      // vertices are generated in world space while the entity transform stays
      // at the origin.
      if (!viewModelPass && mCullingEnabled &&
          (!shadowPass || mShadowCameraCulling)) {
        ++mStats.tested;
        glm::mat4 world = worldMatrix(worldMatrix, entity);
        float radius = 1.0f;
        glm::vec3 centerOffset(0.0f);
        if (registry.has<BoundsComponent>(entity))
        {
          const auto &bounds = registry.get<BoundsComponent>(entity);
          centerOffset = bounds.centerOffset;
          radius = bounds.radius;
        }
        const glm::vec3 center = glm::vec3(world[3]) + centerOffset;

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

        if (mesh.isTerrain && !mesh.isWater) {
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

        if (mesh.isTerrain && !mesh.isWater) {
          shader.setBool("uTerrainPass", false);
        }
      }
    }
    }

    // ------------------------------------------------------------------
    // Draw Instanced Meshes
    // ------------------------------------------------------------------
    if (viewModelPass)
      return;
    const uint64_t frameCullKey = buildCullKey_(shadowPass);
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
      const float maxDist =
          shadowPass ? std::min(inst.maxDrawDistance, inst.shadowMaxDrawDistance)
                     : inst.maxDrawDistance;
      const float shadowDistanceLimit =
          shadowPass ? mShadowDistanceLimit
                     : std::numeric_limits<float>::infinity();
      const float cappedMaxDist =
          std::isfinite(shadowDistanceLimit)
              ? std::min(maxDist, shadowDistanceLimit)
              : maxDist;
      const float maxDist2 = cappedMaxDist * cappedMaxDist;
      const float baseRadius = std::max(0.25f, inst.instanceCullRadius);

      if (inst.isDirty) {
        inst.clusterDataDirty = true;
        inst.mainCacheDirty = true;
        inst.shadowCacheDirty = true;
        inst.isDirty = false;
      }
      if (inst.clusterDataDirty)
        buildInstanceClusters_(inst);

      std::vector<glm::mat4> &cachedTransforms =
          shadowPass ? inst.shadowCulledTransforms : inst.culledTransforms;
      uint64_t &lastCullKey =
          shadowPass ? inst.shadowLastCullKey : inst.lastCullKey;
      int &lastVisibleCount =
          shadowPass ? inst.shadowLastVisibleCount : inst.lastVisibleCount;
      int &lastTestedClusterCount = shadowPass
                                        ? inst.shadowLastTestedClusterCount
                                        : inst.lastTestedClusterCount;
      int &lastVisibleClusterCount = shadowPass
                                         ? inst.shadowLastVisibleClusterCount
                                         : inst.lastVisibleClusterCount;
      bool &cacheDirty =
          shadowPass ? inst.shadowCacheDirty : inst.mainCacheDirty;
      unsigned int &targetVBO =
          shadowPass ? inst.shadowInstanceVBO : inst.instanceVBO;
      size_t &targetCapacity =
          shadowPass ? inst.shadowInstanceVBOCapacity
                     : inst.instanceVBOCapacity;

      const int totalCount = (int)inst.instanceTransforms.size();
      const int cachedVisibleCount = lastVisibleCount;
      int visibleCount = cachedVisibleCount;
      const bool needsRebuild =
          cacheDirty || lastCullKey != frameCullKey || targetVBO == 0;
      if (needsRebuild) {
        cachedTransforms.clear();
        cachedTransforms.reserve(totalCount);
        const float r = shadowPass ? baseRadius * 1.5f : baseRadius;
        const float clusterFullDist =
            std::max(0.0f, cappedMaxDist - r);
        const float clusterFullDist2 = clusterFullDist * clusterFullDist;
        int testedClusters = 0;
        int visibleClusters = 0;

        for (const auto &cluster : inst.instanceClusters) {
          ++testedClusters;
          const float clusterRadius = cluster.radius + r;
          const glm::vec3 clusterDelta = cluster.center - mCameraPos;
          const float clusterDist2 =
              clusterDelta.x * clusterDelta.x + clusterDelta.y * clusterDelta.y +
              clusterDelta.z * clusterDelta.z;
          const float clusterCullDist = cappedMaxDist + clusterRadius;
          if (clusterDist2 > clusterCullDist * clusterCullDist) {
            if (shadowPass)
              recordShadowCasterCull_((int)cluster.indexCount, false);
            continue;
          }
          if (shadowPass &&
              shouldCullSmallShadowCaster_(std::sqrt(clusterDist2),
                                           clusterRadius)) {
            recordShadowCasterCull_((int)cluster.indexCount, true);
            continue;
          }
          if (!sphereInFrustum_(cluster.center, clusterRadius))
            continue;

          ++visibleClusters;
          const bool fullyInsideFrustum =
              sphereFullyInsideFrustum_(cluster.center, clusterRadius);
          const bool fullyInsideDistance = clusterDist2 <= clusterFullDist2;
          if (fullyInsideFrustum && fullyInsideDistance) {
            const uint32_t offset = cluster.indexOffset;
            const uint32_t end = offset + cluster.indexCount;
            for (uint32_t i = offset; i < end; ++i) {
              cachedTransforms.push_back(
                  inst.instanceTransforms[inst.clusterInstanceIndices[i]]);
            }
            continue;
          }

          const uint32_t offset = cluster.indexOffset;
          const uint32_t end = offset + cluster.indexCount;
          for (uint32_t i = offset; i < end; ++i) {
            const glm::mat4 &m =
                inst.instanceTransforms[inst.clusterInstanceIndices[i]];
            const glm::vec3 worldPos(m[3]);
            glm::vec3 d = worldPos - mCameraPos;
            if (d.x * d.x + d.y * d.y + d.z * d.z > maxDist2)
              continue;
            bool visible = true;
            for (const glm::vec4 &p : mFrustumPlanes) {
              if (glm::dot(glm::vec3(p), worldPos) + p.w < -r) {
                visible = false;
                break;
              }
            }
            if (visible)
              cachedTransforms.push_back(m);
          }
        }
        recordClusterStats_(shadowPass, testedClusters, visibleClusters);
        lastTestedClusterCount = testedClusters;
        lastVisibleClusterCount = visibleClusters;

        visibleCount = (int)cachedTransforms.size();
      }
      else {
        recordClusterStats_(shadowPass, lastTestedClusterCount,
                            lastVisibleClusterCount);
      }

      if (visibleCount == 0) {
        if (needsRebuild) {
          lastVisibleCount = 0;
          lastCullKey = frameCullKey;
          cacheDirty = false;
        }
        if (!shadowPass)
          mStats.culled += totalCount;
        shader.setBool("uInstanced", false);
        continue;
      }

      if (needsRebuild) {
        const size_t neededBytes = (size_t)visibleCount * sizeof(glm::mat4);
        const bool canReusePrevious =
            !cacheDirty && targetVBO != 0 && cachedVisibleCount > 0;
        const bool budgetAccepted = tryConsumeUploadBudget_(neededBytes);
        if (!budgetAccepted && canReusePrevious) {
          visibleCount = cachedVisibleCount;
          recordInstanceUploadSkip_(shadowPass);
        } else {
          size_t uploadedBytes = 0;
          if (uploadInstanceBuffer_(targetVBO, targetCapacity,
                                    cachedTransforms.data(), visibleCount,
                                    uploadedBytes)) {
            if (!budgetAccepted)
              mUploadBytesUsed += uploadedBytes;
            recordInstanceUpload_(shadowPass, uploadedBytes);
          } else {
            recordInstanceUploadSkip_(shadowPass);
          }
          lastVisibleCount = visibleCount;
          lastCullKey = frameCullKey;
          cacheDirty = false;
        }
      } else {
        recordInstanceUploadSkip_(shadowPass);
      }

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
          inst.objModel->drawDepthInstanced(
              shader, shadowPass ? inst.shadowInstanceVBO : inst.instanceVBO,
                                            visibleCount);
        } else if (inst.ufbxModel) {
          inst.ufbxModel->drawDepthInstanced(
              shader, shadowPass ? inst.shadowInstanceVBO : inst.instanceVBO,
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
  struct FramePacket {
    EntityId entity = 0;
    uint64_t sortKey = 0;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius = 1.0f;
    float lodMinDistance = 0.0f;
    float lodMaxDistance = std::numeric_limits<float>::max();
    int submeshCount = 0;
    bool hasLod = false;
    bool castsShadow = true;
    bool isTerrain = false;
    bool isWater = false;
    bool isViewModel = false;
    class OBJModel *objModel = nullptr;
    class FBXModel *gltfModel = nullptr;
    class UFBXModel *ufbxModel = nullptr;
  };

  // Scene loading stores material-override texture paths only (it is
  // GL-free); the GL layer turns them into textures here, once.
  static void resolveMaterialOverrideTextures_(MaterialOverrideComponent &mo) {
    if (mo.texturesResolved)
      return;
    mo.texturesResolved = true;
    auto resolve = [](uint32_t &id, const std::string &path,
                      TextureUsage usage) {
      if (id == 0 && !path.empty())
        id = LoadTexture2DCached(path, true, usage);
    };
    resolve(mo.material.texDiffuse, mo.albedoPath, TextureUsage::Color);
    resolve(mo.material.texNormal, mo.normalPath, TextureUsage::Data);
    resolve(mo.material.texRoughness, mo.roughnessPath, TextureUsage::Data);
    resolve(mo.material.texMetallic, mo.metallicPath, TextureUsage::Data);
    resolve(mo.material.texAO, mo.aoPath, TextureUsage::Data);
  }

  void prepareFramePackets_(Registry &registry) {
    if (mFramePacketsPrepared)
      return;
    mFramePacketsPrepared = true;

    mWorldCache.clear();
    mVisit.clear();
    mFramePackets.clear();
    mEntityPacketIndex.clear();

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
      if (!mesh.objModel && !mesh.gltfModel && !mesh.ufbxModel)
        continue;

      FramePacket packet;
      packet.entity = entity;
      packet.objModel = mesh.objModel;
      packet.gltfModel = mesh.gltfModel;
      packet.ufbxModel = mesh.ufbxModel;
      packet.castsShadow = mesh.castsShadow;
      packet.isTerrain = mesh.isTerrain;
      packet.isWater = mesh.isWater;
      packet.isViewModel = mesh.isViewModel;

      glm::mat4 world = worldMatrix(worldMatrix, entity);
      packet.position = glm::vec3(world[3]);
      packet.scale = glm::vec3(glm::length(glm::vec3(world[0])),
                               glm::length(glm::vec3(world[1])),
                               glm::length(glm::vec3(world[2])));
      glm::mat4 rotMat = world;
      if (packet.scale.x > 1e-6f)
        rotMat[0] /= packet.scale.x;
      if (packet.scale.y > 1e-6f)
        rotMat[1] /= packet.scale.y;
      if (packet.scale.z > 1e-6f)
        rotMat[2] /= packet.scale.z;
      packet.rotation = glm::degrees(glm::eulerAngles(glm::quat_cast(rotMat)));

      if (registry.has<BoundsComponent>(entity)) {
        const auto &bounds = registry.get<BoundsComponent>(entity);
        packet.boundsCenter = packet.position + bounds.centerOffset;
        packet.boundsRadius = bounds.radius;
      } else {
        packet.boundsCenter = packet.position;
      }

      if (registry.has<LODComponent>(entity)) {
        const auto &lod = registry.get<LODComponent>(entity);
        packet.hasLod = true;
        packet.lodMinDistance = lod.minDistance;
        packet.lodMaxDistance = lod.maxDistance;
      }

      if (mesh.objModel)
        packet.submeshCount = (int)mesh.objModel->submeshCount();
      else if (mesh.gltfModel)
        packet.submeshCount = (int)mesh.gltfModel->submeshCount();
      else if (mesh.ufbxModel)
        packet.submeshCount = (int)mesh.ufbxModel->submeshCount();

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
        resolveMaterialOverrideTextures_(mo);
        if (mo.enabled) {
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

      packet.sortKey = materialKey;
      packet.sortKey ^= modelKey + 0x9e3779b97f4a7c15ull +
                        (packet.sortKey << 6) + (packet.sortKey >> 2);
      mFramePackets.push_back(packet);
    }

    std::sort(mFramePackets.begin(), mFramePackets.end(),
              [](const FramePacket &a, const FramePacket &b) {
                if (a.sortKey != b.sortKey)
                  return a.sortKey < b.sortKey;
                return a.entity < b.entity;
              });

    for (size_t i = 0; i < mFramePackets.size(); ++i) {
      mEntityPacketIndex[mFramePackets[i].entity] = i;
    }
  }

  bool updateMeshPackets_(Registry &registry, Shader &shader, bool shadowPass,
                          EntityId selectedEntity, bool outlinePass,
                          bool viewModelPass, TerrainFilter terrainFilter) {
    prepareFramePackets_(registry);

    auto passMatches = [&](const FramePacket &packet) -> bool {
      if (viewModelPass) {
        if (!packet.isViewModel)
          return false;
      } else if (packet.isViewModel) {
        return false;
      }
      if (shadowPass && !packet.castsShadow)
        return false;
      if (terrainFilter == TerrainFilter::OnlyTerrain &&
          (!packet.isTerrain || packet.isWater))
        return false;
      if (terrainFilter == TerrainFilter::ExcludeTerrain &&
          (packet.isTerrain && !packet.isWater))
        return false;
      return true;
    };

    auto packetVisible = [&](const FramePacket &packet) -> bool {
      if (viewModelPass)
        return true;
      if (!mCullingEnabled || (shadowPass && !mShadowCameraCulling))
        return true;

      ++mStats.tested;
      if (shadowPass && std::isfinite(mShadowDistanceLimit)) {
        const float centerDist = glm::length(mCameraPos - packet.boundsCenter);
        if (centerDist - packet.boundsRadius > mShadowDistanceLimit) {
          ++mStats.culled;
          recordShadowCasterCull_(1, false);
          return false;
        }
        if (shouldCullSmallShadowCaster_(centerDist, packet.boundsRadius)) {
          ++mStats.culled;
          recordShadowCasterCull_(1, true);
          return false;
        }
      }
      if (packet.hasLod) {
        const float d = glm::length(mCameraPos - packet.boundsCenter);
        if (d < packet.lodMinDistance || d > packet.lodMaxDistance) {
          ++mStats.culled;
          return false;
        }
      }
      if (!sphereInFrustum_(packet.boundsCenter, packet.boundsRadius)) {
        ++mStats.culled;
        return false;
      }
      return true;
    };

    auto drawPacket = [&](const FramePacket &packet, bool enableStencilWrite) {
      if (shadowPass) {
        mStats.drawCallsShadow += packet.submeshCount;
        if (packet.objModel) {
          packet.objModel->drawDepth(shader, packet.position, packet.rotation,
                                     packet.scale);
        } else if (packet.gltfModel) {
          packet.gltfModel->drawDepth(shader, packet.position, packet.rotation,
                                      packet.scale);
        } else if (packet.ufbxModel) {
          packet.ufbxModel->drawDepth(shader, packet.position, packet.rotation,
                                      packet.scale);
        }
        return;
      }

      mStats.drawCallsMain += packet.submeshCount;
      if (enableStencilWrite && selectedEntity != 0) {
        if (packet.entity == selectedEntity) {
          glStencilFunc(GL_ALWAYS, 1, 0xFF);
          glStencilMask(0xFF);
        } else {
          glStencilMask(0x00);
        }
      }

      if (packet.isTerrain && !packet.isWater) {
        shader.setBool("uTerrainPass", true);
        shader.setBool("uUseColor", false);
      }

      const MaterialAsset *materialOverride = nullptr;
      if (registry.has<MaterialOverrideComponent>(packet.entity)) {
        auto &mo = registry.get<MaterialOverrideComponent>(packet.entity);
        if (mo.enabled)
          materialOverride = &mo.material;
      }

      if (packet.objModel) {
        packet.objModel->draw(shader, packet.position, packet.rotation,
                              packet.scale, materialOverride);
        ++mStats.drawn;
      } else if (packet.gltfModel) {
        packet.gltfModel->draw(shader, packet.position, packet.rotation,
                               packet.scale, materialOverride);
        ++mStats.drawn;
      } else if (packet.ufbxModel) {
        packet.ufbxModel->draw(shader, packet.position, packet.rotation,
                               packet.scale, materialOverride);
        ++mStats.drawn;
      }

      if (packet.isTerrain && !packet.isWater) {
        shader.setBool("uTerrainPass", false);
      }
    };

    if (outlinePass) {
      auto it = mEntityPacketIndex.find(selectedEntity);
      if (it != mEntityPacketIndex.end()) {
        const FramePacket &packet = mFramePackets[it->second];
        if (passMatches(packet) && packetVisible(packet))
          drawPacket(packet, false);
      }
    } else {
      for (const FramePacket &packet : mFramePackets) {
        if (!passMatches(packet))
          continue;
        if (!packetVisible(packet))
          continue;
        drawPacket(packet, true);
      }
    }
    return true;
  }

  void recordClusterStats_(bool shadowPass, int tested, int visible) {
    if (shadowPass) {
      mStats.instancedClustersTestedShadow += tested;
      mStats.instancedClustersVisibleShadow += visible;
    } else {
      mStats.instancedClustersTestedMain += tested;
      mStats.instancedClustersVisibleMain += visible;
    }
  }

  void recordShadowCasterCull_(int count, bool smallCaster) {
    if (count <= 0)
      return;
    if (smallCaster) {
      mStats.shadowSmallCasterCulled += count;
    } else {
      mStats.shadowDistanceCulled += count;
    }
  }

  bool shouldCullSmallShadowCaster_(float centerDistance, float radius) const {
    if (!std::isfinite(mShadowDistanceLimit))
      return false;

    const float safeDistance = std::max(centerDistance, 1.0f);
    const float safeRadius = std::max(radius, 0.05f);
    if (safeDistance < mShadowDistanceLimit * 0.38f)
      return false;

    const float projectedRadius = safeRadius / safeDistance;
    const float distanceAlpha =
        std::clamp(safeDistance / std::max(mShadowDistanceLimit, 1.0f), 0.0f,
                   1.0f);
    const float minUsefulRadius = 0.18f + distanceAlpha * 0.92f;
    const float minProjectedRadius = 0.00115f;
    return safeRadius < minUsefulRadius &&
           projectedRadius < minProjectedRadius;
  }

  bool sphereFullyInsideFrustum_(const glm::vec3 &center, float radius) const {
    for (const glm::vec4 &p : mFrustumPlanes) {
      if (glm::dot(glm::vec3(p), center) + p.w < radius)
        return false;
    }
    return true;
  }

  void buildInstanceClusters_(InstancedMeshComponent &inst) const {
    inst.instanceClusters.clear();
    inst.clusterInstanceIndices.clear();
    inst.clusterDataDirty = false;

    const size_t count = inst.instanceTransforms.size();
    if (count == 0)
      return;

    struct ClusterCellKey {
      int x = 0;
      int y = 0;
      int z = 0;

      bool operator==(const ClusterCellKey &other) const {
        return x == other.x && y == other.y && z == other.z;
      }
    };

    struct ClusterCellKeyHash {
      size_t operator()(const ClusterCellKey &key) const {
        size_t h = (size_t)1469598103934665603ull;
        auto mix = [&](uint64_t v) {
          h ^= (size_t)(v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
        };
        mix((uint64_t)(uint32_t)key.x);
        mix((uint64_t)(uint32_t)key.y);
        mix((uint64_t)(uint32_t)key.z);
        return h;
      }
    };

    const float cellSize =
        std::clamp(std::max(inst.instanceCullRadius * 4.0f, 8.0f), 8.0f, 48.0f);
    const float invCell = 1.0f / std::max(cellSize, 0.001f);
    const float verticalCell = cellSize * 1.5f;
    const float invVerticalCell = 1.0f / std::max(verticalCell, 0.001f);

    std::unordered_map<ClusterCellKey, std::vector<uint32_t>, ClusterCellKeyHash>
        buckets;
    buckets.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
      const glm::vec3 pos(inst.instanceTransforms[i][3]);
      const ClusterCellKey key{
          (int)std::floor(pos.x * invCell),
          (int)std::floor(pos.y * invVerticalCell),
          (int)std::floor(pos.z * invCell),
      };
      buckets[key].push_back(i);
    }

    inst.instanceClusters.reserve(buckets.size());
    inst.clusterInstanceIndices.reserve(count);
    for (auto &entry : buckets) {
      auto &indices = entry.second;
      if (indices.empty())
        continue;

      glm::vec3 center(0.0f);
      for (uint32_t idx : indices) {
        center += glm::vec3(inst.instanceTransforms[idx][3]);
      }
      center /= (float)indices.size();

      float radius = 0.0f;
      for (uint32_t idx : indices) {
        radius = std::max(
            radius, glm::length(glm::vec3(inst.instanceTransforms[idx][3]) - center));
      }

      InstancedMeshComponent::InstanceCluster cluster;
      cluster.center = center;
      cluster.radius = radius;
      cluster.indexOffset = (uint32_t)inst.clusterInstanceIndices.size();
      cluster.indexCount = (uint32_t)indices.size();
      inst.clusterInstanceIndices.insert(inst.clusterInstanceIndices.end(),
                                         indices.begin(), indices.end());
      inst.instanceClusters.push_back(cluster);
    }
  }

  bool tryConsumeUploadBudget_(size_t bytes) {
    if (bytes == 0)
      return true;
    if (mUploadBudgetBytes == std::numeric_limits<size_t>::max()) {
      mUploadBytesUsed += bytes;
      return true;
    }
    if (mUploadBytesUsed + bytes > mUploadBudgetBytes)
      return false;
    mUploadBytesUsed += bytes;
    return true;
  }

  void recordInstanceUpload_(bool shadowPass, size_t bytes) {
    const int safeBytes = (int)std::min<size_t>(
        bytes, (size_t)std::numeric_limits<int>::max());
    if (shadowPass) {
      ++mStats.instancedUploadsShadow;
      mStats.instancedUploadBytesShadow += safeBytes;
    } else {
      ++mStats.instancedUploadsMain;
      mStats.instancedUploadBytesMain += safeBytes;
    }
  }

  void recordInstanceUploadSkip_(bool shadowPass) {
    if (shadowPass) {
      ++mStats.instancedUploadSkipsShadow;
    } else {
      ++mStats.instancedUploadSkipsMain;
    }
  }

  bool uploadInstanceBuffer_(unsigned int &targetVBO, size_t &targetCapacity,
                             const glm::mat4 *data, int visibleCount,
                             size_t &uploadedBytes) const {
    uploadedBytes = 0;
    if (!data || visibleCount <= 0)
      return false;

    if (targetVBO == 0)
      glGenBuffers(1, &targetVBO);

    const size_t neededBytes = (size_t)visibleCount * sizeof(glm::mat4);
    glBindBuffer(GL_ARRAY_BUFFER, targetVBO);
    if (neededBytes > targetCapacity) {
      glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)neededBytes, nullptr,
                   GL_DYNAMIC_DRAW);
      targetCapacity = neededBytes;
    }

    bool uploaded = false;
    if (mSubmissionBackend == RenderSubmissionBackend::Modern &&
        glMapBufferRange != nullptr) {
      void *mapped =
          glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)neededBytes,
                           GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
      if (mapped) {
        std::memcpy(mapped, data, neededBytes);
        uploaded = (glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE);
      }
    }
    if (!uploaded) {
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)neededBytes, data);
      uploaded = true;
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (uploaded)
      uploadedBytes = neededBytes;
    return uploaded;
  }

  uint64_t buildCullKey_(bool shadowPass) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
      h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    };
    auto quant = [](float v, float scale) -> int64_t {
      return (int64_t)std::llround(v * scale);
    };
    const float cameraScale = shadowPass ? 2.0f : 4.0f;
    const float planeScale = shadowPass ? 128.0f : 256.0f;
    mix((uint64_t)shadowPass);
    // Quantize camera position and frustum planes more coarsely so tiny camera
    // jitter does not thrash instance uploads.
    mix((uint64_t)quant(mCameraPos.x, cameraScale));
    mix((uint64_t)quant(mCameraPos.y, cameraScale));
    mix((uint64_t)quant(mCameraPos.z, cameraScale));
    for (const glm::vec4 &p : mFrustumPlanes) {
      mix((uint64_t)quant(p.x, planeScale));
      mix((uint64_t)quant(p.y, planeScale));
      mix((uint64_t)quant(p.z, planeScale));
      mix((uint64_t)quant(p.w, cameraScale));
    }
    return h;
  }

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
  bool mShadowCameraCulling = true;
  VisibilityStats mStats{};
  RenderSubmissionBackend mSubmissionBackend =
      RenderSubmissionBackend::Direct;
  size_t mUploadBudgetBytes = std::numeric_limits<size_t>::max();
  size_t mUploadBytesUsed = 0;
  float mShadowDistanceLimit = std::numeric_limits<float>::infinity();

  // Persistent per-frame caches — cleared each frame, capacity retained
  std::unordered_map<EntityId, glm::mat4> mWorldCache;
  std::unordered_map<EntityId, uint8_t> mVisit;
  std::vector<DrawItem> mDrawList;
  std::vector<FramePacket> mFramePackets;
  std::unordered_map<EntityId, size_t> mEntityPacketIndex;
  bool mFramePacketsPrepared = false;
  // Scratch buffer for per-instance frustum culling (avoids malloc each frame)
};

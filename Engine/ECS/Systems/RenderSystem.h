#pragma once
#include "Assets/FBXModel.h"
#include "Assets/OBJModel.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Rendering/Shader.h"
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
  };

  void setViewProjection(const glm::mat4 &vp) { mViewProjection = vp; }
  void setCameraPosition(const glm::vec3 &p) { mCameraPos = p; }
  void setCullingEnabled(bool enabled) { mCullingEnabled = enabled; }
  bool cullingEnabled() const { return mCullingEnabled; }
  const VisibilityStats &stats() const { return mStats; }

  void update(Registry &registry, Shader &shader, bool shadowPass = false) {
    if (!shadowPass)
      mStats = {};

    // Set pass-level uniforms ONCE (instead of per-object in OBJModel::draw)
    if (!shadowPass) {
      shader.setBool("uGlowPass", false);
      shader.setBool("uCloudPass", false);
      shader.setInt("texture1", 0);
    }

    std::unordered_map<EntityId, glm::mat4> worldCache;
    std::unordered_map<EntityId, uint8_t> visit;

    auto worldMatrix = [&](auto &&self, EntityId e) -> glm::mat4 {
      auto itV = visit.find(e);
      if (itV != visit.end() && itV->second == 2)
        return worldCache[e];
      if (itV != visit.end() && itV->second == 1)
        return registry.get<TransformComponent>(e).getMatrix();

      visit[e] = 1;
      glm::mat4 local = registry.get<TransformComponent>(e).getMatrix();
      glm::mat4 world = local;

      if (registry.has<HierarchyComponent>(e)) {
        auto &h = registry.get<HierarchyComponent>(e);
        if (h.parent != 0 && registry.has<TransformComponent>(h.parent)) {
          world = self(self, h.parent) * local;
        }
      }

      visit[e] = 2;
      worldCache[e] = world;
      return world;
    };

    auto drawWithWorld = [&](EntityId entity, auto drawFn) {
      glm::mat4 world = worldMatrix(worldMatrix, entity);

      if (!shadowPass && mCullingEnabled) {
        ++mStats.tested;
        float radius = 1.0f;
        if (registry.has<BoundsComponent>(entity))
          radius = registry.get<BoundsComponent>(entity).radius;
        const glm::vec3 center = glm::vec3(world[3]);

        if (registry.has<LODComponent>(entity)) {
          const auto &lod = registry.get<LODComponent>(entity);
          const float d = glm::length(mCameraPos - center);
          if (d < lod.minDistance || d > lod.maxDistance) {
            ++mStats.culled;
            return;
          }
        }
        if (!sphereInFrustum_(center, radius)) {
          ++mStats.culled;
          return;
        }
      }

      glm::vec3 skew;
      glm::vec4 perspective;
      glm::vec3 pos, scale;
      glm::quat rotQ;
      if (!glm::decompose(world, scale, rotQ, pos, skew, perspective))
        return;
      glm::vec3 rot = glm::degrees(glm::eulerAngles(rotQ));
      drawFn(pos, rot, scale);
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
      if (shadowPass && !mesh.castsShadow)
        continue;
      if (!mesh.objModel && !mesh.fbxModel)
        continue;

      if (shadowPass) {
        if (mesh.objModel) {
          drawWithWorld(entity, [&](const glm::vec3 &p, const glm::vec3 &r,
                                    const glm::vec3 &s) {
            mesh.objModel->drawDepth(shader, p, r, s);
            if (!shadowPass)
              ++mStats.drawn;
          });
        } else if (mesh.fbxModel) {
          drawWithWorld(entity, [&](const glm::vec3 &p, const glm::vec3 &r,
                                    const glm::vec3 &s) {
            mesh.fbxModel->drawDepth(shader, p, r, s);
            if (!shadowPass)
              ++mStats.drawn;
          });
        }
      } else {
        if (mesh.objModel) {
          drawWithWorld(entity, [&](const glm::vec3 &p, const glm::vec3 &r,
                                    const glm::vec3 &s) {
            mesh.objModel->draw(shader, p, r, s);
            ++mStats.drawn;
          });
        } else if (mesh.fbxModel) {
          drawWithWorld(entity, [&](const glm::vec3 &p, const glm::vec3 &r,
                                    const glm::vec3 &s) {
            mesh.fbxModel->draw(shader, p, r, s);
            ++mStats.drawn;
          });
        }
      }
    }
  }

private:
  bool sphereInFrustum_(const glm::vec3 &center, float radius) const {
    const glm::vec4 r0(mViewProjection[0][0], mViewProjection[1][0],
                       mViewProjection[2][0], mViewProjection[3][0]);
    const glm::vec4 r1(mViewProjection[0][1], mViewProjection[1][1],
                       mViewProjection[2][1], mViewProjection[3][1]);
    const glm::vec4 r2(mViewProjection[0][2], mViewProjection[1][2],
                       mViewProjection[2][2], mViewProjection[3][2]);
    const glm::vec4 r3(mViewProjection[0][3], mViewProjection[1][3],
                       mViewProjection[2][3], mViewProjection[3][3]);

    const glm::vec4 planes[6] = {
        r3 + r0, // left
        r3 - r0, // right
        r3 + r1, // bottom
        r3 - r1, // top
        r3 + r2, // near
        r3 - r2  // far
    };

    for (const glm::vec4 &p : planes) {
      const float len = glm::length(glm::vec3(p));
      if (len <= 0.00001f)
        continue;
      const float d = (glm::dot(glm::vec3(p), center) + p.w) / len;
      if (d < -radius)
        return false;
    }
    return true;
  }

  glm::mat4 mViewProjection{1.0f};
  glm::vec3 mCameraPos{0.0f};
  bool mCullingEnabled = true;
  VisibilityStats mStats{};
};

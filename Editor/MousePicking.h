#pragma once
#include "../Engine/Assets/OBJModel.h"
#include "../Engine/ECS/Components.h"
#include "../Engine/ECS/Registry.h"
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <string>
#include <vector>

// =============================================================================
// Mouse Picking — Raycasting for entity and submesh selection
// =============================================================================

struct Ray {
  glm::vec3 origin;
  glm::vec3 direction; // normalized
};

namespace MousePicking {

/// Convert screen coordinates (pixels) to a world-space ray.
inline Ray screenToRay(float mouseX, float mouseY, float viewportX,
                       float viewportY, float viewportW, float viewportH,
                       const glm::mat4 &view, const glm::mat4 &projection) {
  float ndcX = (2.0f * (mouseX - viewportX) / viewportW) - 1.0f;
  float ndcY = 1.0f - (2.0f * (mouseY - viewportY) / viewportH);

  glm::mat4 invVP = glm::inverse(projection * view);
  glm::vec4 nearPoint = invVP * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
  glm::vec4 farPoint = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);

  nearPoint /= nearPoint.w;
  farPoint /= farPoint.w;

  glm::vec3 origin = glm::vec3(nearPoint);
  glm::vec3 direction = glm::normalize(glm::vec3(farPoint) - origin);
  return {origin, direction};
}

/// Ray-sphere intersection. Returns distance or -1 on miss.
inline float raySphereIntersect(const Ray &ray, const glm::vec3 &center,
                                float radius) {
  glm::vec3 oc = ray.origin - center;
  float a = glm::dot(ray.direction, ray.direction);
  float b = 2.0f * glm::dot(oc, ray.direction);
  float c = glm::dot(oc, oc) - radius * radius;
  float discriminant = b * b - 4.0f * a * c;

  if (discriminant < 0.0f)
    return -1.0f;

  float sqrtD = std::sqrt(discriminant);
  float t1 = (-b - sqrtD) / (2.0f * a);
  float t2 = (-b + sqrtD) / (2.0f * a);

  if (t1 > 0.0f)
    return t1;
  if (t2 > 0.0f)
    return t2;
  return -1.0f;
}

/// Ray-AABB (slab method). Returns distance or -1 on miss.
inline float rayAABBIntersect(const Ray &ray, const glm::vec3 &aabbMin,
                              const glm::vec3 &aabbMax) {
  float tMin = -std::numeric_limits<float>::max();
  float tMax = std::numeric_limits<float>::max();

  for (int i = 0; i < 3; ++i) {
    float invD = 1.0f / ray.direction[i];
    float t0 = (aabbMin[i] - ray.origin[i]) * invD;
    float t1 = (aabbMax[i] - ray.origin[i]) * invD;
    if (invD < 0.0f)
      std::swap(t0, t1);
    tMin = t0 > tMin ? t0 : tMin;
    tMax = t1 < tMax ? t1 : tMax;
    if (tMax < tMin)
      return -1.0f;
  }
  return tMin > 0.0f ? tMin : (tMax > 0.0f ? tMax : -1.0f);
}

/// Pick the closest entity under the mouse cursor (bounding sphere).
inline uint32_t pickEntity(const Ray &ray, Registry &reg) {
  uint32_t bestEntity = 0;
  float bestDist = std::numeric_limits<float>::max();

  auto &transforms = reg.view<TransformComponent>();
  for (auto entity : transforms) {
    if (!reg.has<BoundsComponent>(entity))
      continue;
    if (!reg.has<MeshComponent>(entity))
      continue;

    auto &tr = reg.get<TransformComponent>(entity);
    auto &bc = reg.get<BoundsComponent>(entity);

    float maxScale = std::max({tr.scale.x, tr.scale.y, tr.scale.z});
    float worldRadius = bc.radius * maxScale;

    float t = raySphereIntersect(ray, tr.position, worldRadius);
    if (t > 0.0f && t < bestDist) {
      bestDist = t;
      bestEntity = static_cast<uint32_t>(entity);
    }
  }
  return bestEntity;
}

/// Pick the closest submesh/object within a specific entity's OBJ model.
/// The ray is transformed into the entity's local space, then tested against
/// each object's AABB. Returns the object name of the closest hit, or empty.
inline std::string pickSubmesh(const Ray &ray, const TransformComponent &tr,
                               OBJModel &model) {
  // Build entity world matrix and its inverse
  glm::mat4 entityMat = tr.getMatrix();
  glm::mat4 invEntity = glm::inverse(entityMat);

  // Transform ray into local model space
  glm::vec3 localOrigin = glm::vec3(invEntity * glm::vec4(ray.origin, 1.0f));
  glm::vec3 localDir =
      glm::normalize(glm::vec3(invEntity * glm::vec4(ray.direction, 0.0f)));
  Ray localRay{localOrigin, localDir};

  auto names = model.objectNames();
  std::string bestName;
  float bestDist = std::numeric_limits<float>::max();

  for (const auto &name : names) {
    glm::vec3 aabbMin, aabbMax;
    if (!model.getObjectBounds(name, aabbMin, aabbMax))
      continue;

    float t = rayAABBIntersect(localRay, aabbMin, aabbMax);
    if (t > 0.0f && t < bestDist) {
      bestDist = t;
      bestName = name;
    }
  }
  return bestName;
}

} // namespace MousePicking

#pragma once
#include "ECS/Components.h"
#include "Terrain/TerrainSettings.h"
#include <algorithm>
#include <cmath>

namespace gameplay {
inline float playerEyeHeight(const TransformComponent &tr, const ColliderComponent &col) {
  if (col.shape != ColliderComponent::Shape::Capsule) return 1.65f;
  const auto scale = glm::abs(tr.scale);
  const float radius = col.dimensions.x * std::max(scale.x, scale.z);
  const float height = std::max(col.dimensions.y * scale.y, radius * 2.f);
  return height * .5f - col.offset.y * tr.scale.y;
}

// Old Player saves contain an offset ABOVE the camera and stretched gizmo
// scales. They cannot be controlled as an eye-position capsule: Jolt places
// the eye below the floor. Restrict this repair to the built-in player caller.
inline bool repairPlayerCapsule(TransformComponent &tr, ColliderComponent &col,
                                RigidbodyComponent &rb) {
  bool repaired = !rb.lockRotation;
  rb.lockRotation = true;
  if (col.shape == ColliderComponent::Shape::Capsule &&
      (col.offset.y >= 0 || glm::length(tr.scale - glm::vec3(1)) > .001f)) {
    col.dimensions = {.6f, 1.8f, .6f};
    col.offset = {0, -.72f, 0};
    tr.scale = glm::vec3(1);
    repaired = true;
  }
  return repaired;
}

// Preserve valid authored spawn points. A player left on another map (or in
// deep water) falls back to the current viewport, then a deterministic dry
// clearing. Analytic samples work before streamed Jolt heightfields arrive.
template<class Height, class Water>
glm::vec3 terrainPlayerSpawn(glm::vec3 requested, glm::vec2 viewport,
                            const TerrainSettings &settings, float eyeHeight,
                            Height heightAt, Water waterAt) {
  auto safe = [&](glm::vec2 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    const float radius = settings.authoredWoodland ? std::max(300.f, settings.worldRadius) : settings.worldRadius;
    if ((settings.worldBounded || settings.authoredWoodland) && glm::length(p) >= radius - 8.f) return false;
    const float ground = heightAt(p), water = waterAt(p);
    return std::isfinite(ground) && (!std::isfinite(water) || ground >= water + .15f);
  };
  const glm::vec2 original(requested.x, requested.z);
  if (safe(original)) {
    requested.y = std::isfinite(requested.y) ? std::max(requested.y, heightAt(original) + eyeHeight)
                                            : heightAt(original) + eyeHeight;
    return requested;
  }
  glm::vec2 candidate = viewport;
  if (!safe(candidate)) candidate = settings.authoredWoodland ? glm::vec2(0,12) : glm::vec2(0);
  if (!safe(candidate)) {
    bool found = false;
    for (float radius=8; radius<=512 && !found; radius+=8) {
      for (int i=0; i<32; ++i) {
        const float angle=i*6.28318530718f/32;
        const glm::vec2 p=glm::vec2(std::cos(angle),std::sin(angle))*radius;
        if (safe(p)) { candidate=p; found=true; break; }
      }
    }
  }
  return {candidate.x, heightAt(candidate) + eyeHeight, candidate.y};
}
} // namespace gameplay

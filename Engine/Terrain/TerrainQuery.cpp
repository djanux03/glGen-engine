#include "TerrainQuery.h"

#include <algorithm>
#include <cmath>

float TerrainQuery::heightAt(glm::vec2 worldXZ) const {
  TerrainMacroSample sample = sampleMacro(mNoiseSet, worldXZ, mSettings);
  return computeHeight(sample, mNoiseSet, worldXZ, mSettings, mEdits);
}

BiomeWeights TerrainQuery::biomeWeightsAt(glm::vec2 worldXZ) const {
  TerrainMacroSample sample = sampleMacro(mNoiseSet, worldXZ, mSettings);
  const float height = computeHeight(sample, mNoiseSet, worldXZ, mSettings, mEdits);
  return sampleBiomeWeights(sample, mSettings, height);
}

glm::vec3 TerrainQuery::normalAt(glm::vec2 worldXZ) const {
  const float eps = 0.5f;
  float hL = heightAt(worldXZ - glm::vec2(eps, 0.0f));
  float hR = heightAt(worldXZ + glm::vec2(eps, 0.0f));
  float hD = heightAt(worldXZ - glm::vec2(0.0f, eps));
  float hU = heightAt(worldXZ + glm::vec2(0.0f, eps));
  return glm::normalize(glm::vec3(-(hR - hL), 2.0f * eps, -(hU - hD)));
}

float TerrainQuery::slopeAt(glm::vec2 worldXZ) const {
  glm::vec3 n = normalAt(worldXZ);
  return 1.0f - std::abs(glm::dot(n, glm::vec3(0.0f, 1.0f, 0.0f)));
}

TerrainQuery::RaycastHit TerrainQuery::raycast(glm::vec3 origin, glm::vec3 dir,
                                               float maxDistance,
                                               float stepSize) const {
  RaycastHit result;
  const glm::vec3 d = glm::length(dir) > 0.0f ? glm::normalize(dir) : dir;

  float prevT = 0.0f;
  // Already underground at the ray's start: no meaningful surface crossing
  // to report (matches "no hit" rather than a nonsensical t=0 hit).
  if (origin.y - heightAt(glm::vec2(origin.x, origin.z)) <= 0.0f)
    return result;

  const int steps = static_cast<int>(maxDistance / stepSize) + 1;
  for (int s = 1; s <= steps; ++s) {
    const float t = std::min(static_cast<float>(s) * stepSize, maxDistance);
    const glm::vec3 p = origin + d * t;
    const float diff = p.y - heightAt(glm::vec2(p.x, p.z));

    if (diff <= 0.0f) {
      // Bisection refinement on [prevT, t] to reduce stair-step jitter.
      float lo = prevT, hi = t;
      for (int i = 0; i < 8; ++i) {
        const float mid = 0.5f * (lo + hi);
        const glm::vec3 mp = origin + d * mid;
        const float midDiff = mp.y - heightAt(glm::vec2(mp.x, mp.z));
        if (midDiff <= 0.0f)
          hi = mid;
        else
          lo = mid;
      }
      const glm::vec3 hitPoint = origin + d * hi;
      result.hit = true;
      result.point = hitPoint;
      result.xz = glm::vec2(hitPoint.x, hitPoint.z);
      result.distance = hi;
      return result;
    }

    prevT = t;
    if (t >= maxDistance)
      break;
  }
  return result;
}

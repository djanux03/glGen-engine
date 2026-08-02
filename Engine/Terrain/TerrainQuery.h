#pragma once
// TerrainQuery.h — read-only terrain query API. Phase 5 adds raycast() and
// an optional HeightOffsetGrid so brush picking reflects the current edited
// surface, not just procedural noise (isChunkLoadedAt/clampXZToLoadedRegion
// are still not needed -- edits apply the same whether or not a touched
// chunk happens to be loaded, so they stay out of scope here).

#include "HeightOffsetGrid.h"
#include "TerrainNoise.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"

#include <glm/glm.hpp>

class TerrainQuery {
public:
  TerrainQuery(const TerrainSettings& settings, const TerrainNoiseSet& noiseSet,
              const HeightOffsetGrid* edits = nullptr)
      : mSettings(settings), mNoiseSet(noiseSet), mEdits(edits) {}

  float heightAt(glm::vec2 worldXZ) const;
  // Continuous biome membership at a point (replaces the old single-category
  // biomeAt()/BiomeType -- see TerrainTypes.h's BiomeWeights doc comment).
  BiomeWeights biomeWeightsAt(glm::vec2 worldXZ) const;
  glm::vec3 normalAt(glm::vec2 worldXZ) const;
  float slopeAt(glm::vec2 worldXZ) const;
  // isUnderwater() removed with seaLevel/ocean (R1) -- there is no water
  // plane in the 3-biome system.

  struct RaycastHit {
    bool hit = false;
    glm::vec3 point{0.0f};
    glm::vec2 xz{0.0f};
    float distance = 0.0f;
  };
  // Fixed-step march sampling heightAt() each step, refined with one
  // bisection pass on the bracketing segment once a sign change in
  // (rayY - terrainHeight) is detected -- reduces stair-step jitter without
  // a full binary-search raymarch.
  RaycastHit raycast(glm::vec3 origin, glm::vec3 dir, float maxDistance = 500.0f,
                    float stepSize = 0.5f) const;

private:
  const TerrainSettings& mSettings;
  const TerrainNoiseSet& mNoiseSet;
  const HeightOffsetGrid* mEdits;
};

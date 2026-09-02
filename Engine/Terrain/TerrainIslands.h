#pragma once
// TerrainIslands.h — which landmass is this, and what kind of place is it.
//
// WHY THIS CAN EXIST NOW
// "Which island am I on" is a connected-component question over the whole map,
// and while the world was an endless streamed plane it was unanswerable: there
// is no whole map to label. Bounding the world (TerrainSettings::worldBounded)
// made a one-time global pass legal, and this is it. That is the real payoff of
// the bounded design -- not the boundary itself, but that the generator can
// finally reason about the world as an object instead of only about the point
// under the camera.
//
// WHAT IT BUYS
// Biomes stop being independent noise. Before this, meadow/forest/mountain came
// from three noise fields evaluated per point, which means every island is
// statistically the same island: the same mix, at the same rate, everywhere.
// Assigning an ARCHETYPE per landmass -- and grading archetypes outward from
// spawn -- is what makes a map legible, and what makes travelling to another
// island mean something. The three weights downstream are unchanged, so no
// shader, material splat or scatter rule has to know any of this happened.
//
// Built once at terrain creation, then read-only: chunk builds hit idAt() from
// worker threads and must never mutate it.

#include "TerrainSettings.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

struct TerrainNoiseSet;

// Coarse, deliberately: this classifies landmasses, not coastlines. The
// shoreline detail comes from the continent field itself, which every consumer
// still samples continuously.
constexpr uint16_t kNoIsland = 0xFFFF;

enum class IslandArchetype : uint8_t {
  Meadows,  // gentle, open, high treeline -- the starting country
  Forest,   // rolling and wooded, dense canopy
  Highland, // tall relief, low treeline, bare peaks
  Marsh,    // very flat and low, sparse and wet
  Count
};

const char *islandArchetypeName(IslandArchetype a);

struct IslandInfo {
  uint16_t id = kNoIsland;
  IslandArchetype archetype = IslandArchetype::Meadows;
  glm::vec2 centroid{0.0f};
  float radius = 0.0f;        // metres, centroid to furthest labelled cell
  float areaSqM = 0.0f;
  float distanceFromSpawn = 0.0f; // centroid distance from the origin
  // Per-island character. Multiplies the landform's relief and shifts the
  // treeline, so two islands of the same archetype still differ and two of
  // different archetypes read as different country from across the water.
  float reliefScale = 1.0f;
  float treelineOffset = 0.0f;
  // Bias applied to the three biome weights. Blended with the local noise
  // rather than replacing it, so a forest island keeps its clearings and its
  // rocky top.
  glm::vec3 weightBias{1.0f, 0.0f, 0.0f}; // meadow, forest, mountain
};

class IslandMap {
public:
  // Labels the continent mask into landmasses. Safe to call on an unbounded
  // world -- it simply produces no islands, and every lookup returns the
  // neutral default, which is exactly the pre-island behaviour.
  void build(const TerrainNoiseSet &noiseSet, const TerrainSettings &settings);

  bool valid() const { return mResolution > 0 && !mIslands.empty(); }
  uint16_t idAt(glm::vec2 worldXZ) const;
  // Never null: an unlabelled point (ocean, or an unbounded world) returns a
  // neutral island so callers do not need a branch on every sample.
  const IslandInfo &infoAt(glm::vec2 worldXZ) const;
  const std::vector<IslandInfo> &islands() const { return mIslands; }

private:
  uint32_t mResolution = 0;
  float mWorldSize = 0.0f;   // metres covered per side
  glm::vec2 mOrigin{0.0f};   // world XZ of cell (0,0)
  std::vector<uint16_t> mLabels;
  std::vector<IslandInfo> mIslands;
  IslandInfo mNeutral;
};

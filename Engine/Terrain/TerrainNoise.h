#pragma once
// TerrainNoise.h — the "MeadowHeight" landform + biome-weight pipeline
// (MEADOW_TERRAIN_REVAMP_PLAN.md §4). Kept as TerrainNoise.h/.cpp rather
// than a separate MeadowHeight.h/.cpp file (a deliberate deviation from the
// plan's file list, documented in VULKAN_PORT_STATUS.md) to avoid an
// ~10-file #include-rename ripple with zero functional benefit -- the
// algorithm content below IS the MeadowHeight design in full.
//
// Single height authority for the whole terrain system (mesh gen,
// collision, vegetation clamping, brush raycasting all resolve through
// computeHeight()/sampleHeightGrid()). No ECS/renderer dependency —
// unit-testable in isolation.

#include "PerlinNoise.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"
#include "HeightOffsetGrid.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

struct TerrainNoiseSet {
  // forestNoise/moistureNoise were "temperature"/"moisture" in the old
  // 6-biome system; renamed to reflect their R1 purpose (climate
  // classification is gone, but the noise channels are reused for forest
  // coverage and turf moisture instead of retiring them).
  PerlinNoise base, warp, forestNoise, moistureNoise, erosion, detail;

  explicit TerrainNoiseSet(uint32_t seed)
      : base(seed + 0u * 1009u),
        warp(seed + 1u * 1009u),
        forestNoise(seed + 2u * 1009u),
        moistureNoise(seed + 3u * 1009u),
        erosion(seed + 4u * 1009u),
        detail(seed + 5u * 1009u) {}
};

// Intermediate per-point sample: raw landform fields shared by
// computeHeight() and sampleBiomeWeights() so both read the exact same
// warped-noise evaluation (no drift between "what shaped the ground" and
// "what biome this point is").
struct TerrainMacroSample {
  float warpedX = 0.0f;
  float warpedZ = 0.0f;

  float moisture = 0.5f;       // 0..1, feeds forest weight + (later) turf lushness

  // Mountain-region macro mask stack (plan §4b.1/.4): macroMountain is the
  // 0..1 "how much mountain country is this" driver, ridged so ranges read
  // as connected chains; macroValley is a broad 0..1 valley-floor driver
  // (1 = valley bottom) used both to carve height and to soften wMountain
  // on valley floors inside mountain country; macroRidge is the sharp ridge
  // contribution, already scaled by macroMountain.
  float macroMountain = 0.0f;
  float macroValley = 0.0f;
  float macroRidge = 0.0f;

  float broadShape = 0.0f;     // very-low-freq rolling component
  float baseShape = 0.0f;      // eroded-fbm terrain detail (see erodedFbm in .cpp)
  float ridgeShape = 0.0f;     // sharp ridge/valley detail for mountain relief

  float forestField = 0.0f;    // 0..1 raw forest-coverage noise, pre-treeline/pre-mountain
  float outcropField = 0.0f;   // -1..1 raw ridge noise, organic rock-outcrop edge hint
};

TerrainMacroSample sampleMacro(const TerrainNoiseSet& noiseSet, glm::vec2 worldXZ,
                                const TerrainSettings& settings);

float computeHeight(const TerrainMacroSample& sample, const TerrainNoiseSet& noiseSet,
                     glm::vec2 worldXZ, const TerrainSettings& settings,
                     const HeightOffsetGrid* edits);

// The ONLY biome classification in the system (plan §4b'): three continuous
// weights summing to ~1, computed from fields already produced by
// sampleMacro()/computeHeight() -- no separate climate classifier. `height`
// is the already-computed computeHeight() result at this point (needed for
// the treeline cutoff).
BiomeWeights sampleBiomeWeights(const TerrainMacroSample& sample,
                                const TerrainSettings& settings, float height);

// Ground fields exported per height-grid sample: everything downstream
// (material splatting in R2, scatter gating in R4, per-pixel lighting in
// R3) reads from this rather than re-deriving biome/moisture itself. Slope
// and curvature are deliberately NOT here -- TerrainChunkMesher already
// finite-differences the height grid to build vertex normals, so it derives
// slope/curvature from that same stencil rather than this struct
// duplicating the work; `rockNoise` is this file's independent noise-based
// contribution to the mesher's final rock mask (organic outcrop edges, not
// purely slope-thresholded).
struct TerrainGroundFields {
  float moisture = 0.5f;
  float wMeadow = 1.0f;
  float wForest = 0.0f;
  float wMountain = 0.0f;
  float rockNoise = 0.0f; // 0..1
};

void sampleHeightGrid(const TerrainNoiseSet& noiseSet, const TerrainSettings& settings,
                       glm::vec2 chunkOrigin, float chunkWorldSize, uint32_t samplesPerEdge,
                       const HeightOffsetGrid* edits, std::vector<float>& outHeights,
                       std::vector<TerrainGroundFields>* outFields);

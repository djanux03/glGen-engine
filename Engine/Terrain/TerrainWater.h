#pragma once
// TerrainWater.h — where water is, and how high its surface sits.
//
// The renderer's first water implementation was a single global plane, which
// is all an ocean needs and all a lake cannot use: lakes sit at their own
// altitudes, one per basin. This module is the authority both halves read --
// the generator carves basins from it, the scatter system refuses to plant
// below it, and the renderer uploads a sampled grid of it to shade against.
//
// TWO KINDS OF WATER
//   ocean -- one global sea level. Everything below it is submerged.
//   lakes -- inland bodies on a jittered cellular grid: one cell, one lake,
//            each with its own flat surface.
//
// WHY CELLS RATHER THAN BASIN DETECTION
// The honest way to find a lake is to flood-fill depressions in the
// heightfield, but chunks are built independently on worker threads and a
// flood fill is inherently global -- it would need heights from chunks that
// may not exist yet, and would break the "each chunk generates
// deterministically from its own coordinates" contract the worker pool is
// built on. A cellular grid gives every point an owning cell computable from
// its coordinates alone, so a lake resolves identically no matter which chunk
// (or thread, or run) asks about it.
//
// KEEPING LAKES IN THEIR BASINS
// A lake's surface must be flat AND below its rim, or water spills visibly
// onto a hillside. So a cell only becomes a lake if the terrain across it is
// flat enough (see kLakeFlatnessProbes), and its surface is anchored to the
// LOWEST of those probes rather than to its centre. Both tests need the
// terrain height before any lake carving -- hence baseHeightAt, which is
// computeHeight minus the water step, and which is why the two functions are
// split in TerrainNoise.

#include "TerrainSettings.h"

#include <cstdint>
#include <glm/glm.hpp>

struct TerrainNoiseSet;
// `class`, not `struct`: MSVC folds the class-key into the mangled name of a
// forward-declared type, so declaring this as a struct here while
// HeightOffsetGrid.h defines a class produces two different symbols and an
// unresolved external at link time.
class HeightOffsetGrid;

// Returned where a point has no water above it at all.
constexpr float kNoWater = -1.0e9f;

struct LakeSample {
  bool inLake = false;
  float surfaceY = kNoWater; // flat water surface of the owning lake
  float floorY = 0.0f;       // basin floor the carve drives toward
  float basin = 0.0f;        // 0..1 carve weight, 1 at the cell centre
};

// Small caller-owned memo for per-cell data. A cell's lake test costs several
// base-height evaluations; a 129x129 chunk grid touches only a handful of
// cells, so caching them turns that from per-sample into per-chunk. Kept as a
// caller-owned struct rather than a member cache on purpose: chunk builds run
// on worker threads, and a shared mutable cache here would be a data race.
struct WaterCellCache {
  static constexpr int kCapacity = 16;
  int64_t keys[kCapacity]{};
  float surfaceY[kCapacity]{};
  float floorY[kCapacity]{};
  glm::vec2 centre[kCapacity]{};
  float radius[kCapacity]{};
  bool isLake[kCapacity]{};
  int count = 0;
};

// Resolves TerrainSettings::seaLevel from oceanCoverage by sampling the
// terrain over a wide area and taking the coverage-th percentile of its
// heights. Call once at terrain creation, before any chunk is built; it reads
// BASE heights, so it does not depend on the water shaping it configures.
float resolveSeaLevel(const TerrainNoiseSet &noiseSet, const TerrainSettings &settings);

// The lake (if any) owning this point. `edits` participates because brush
// edits change the terrain a lake is measured against.
LakeSample sampleLake(const TerrainNoiseSet &noiseSet, const TerrainSettings &settings,
                      glm::vec2 worldXZ, const HeightOffsetGrid *edits,
                      WaterCellCache *cache);

// Height of the water surface above this point, or kNoWater if dry. This is
// the function every consumer outside the generator should use.
float waterSurfaceAt(const TerrainNoiseSet &noiseSet, const TerrainSettings &settings,
                     glm::vec2 worldXZ, const HeightOffsetGrid *edits,
                     WaterCellCache *cache);

// Applies the water step to an already-computed base height: carves the lake
// basin, then eases relief near the sea line so coasts get beaches instead of
// cliffs. Called by computeHeight() -- consumers want computeHeight().
float applyWaterToHeight(float baseHeight, const LakeSample &lake,
                         const TerrainSettings &settings);

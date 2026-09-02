#include "TerrainWater.h"

#include "TerrainNoise.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Integer hash. Deliberately not std::mt19937/uniform_real_distribution: the
// generator contract (AGENTS.md) is that the same seed gives byte-identical
// geometry, and the standard distributions are not specified to map engine
// output to values identically across standard libraries.
uint32_t hashCell(int32_t cx, int32_t cz, uint32_t seed) {
  uint32_t h = seed * 0x9E3779B9u;
  h ^= static_cast<uint32_t>(cx) * 0x85EBCA6Bu;
  h = (h ^ (h >> 13)) * 0xC2B2AE35u;
  h ^= static_cast<uint32_t>(cz) * 0x27D4EB2Fu;
  h = (h ^ (h >> 16)) * 0x165667B1u;
  return h ^ (h >> 15);
}

float hashUnit(uint32_t h) {
  // 24 bits of mantissa is plenty and keeps this exactly representable.
  return static_cast<float>(h & 0x00FFFFFFu) / 16777215.0f;
}

float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Base height WITHOUT any water carving -- see the header on why this has to
// exist separately.
float baseHeightAt(const TerrainNoiseSet &n, const TerrainSettings &s, glm::vec2 xz,
                   const HeightOffsetGrid *edits) {
  const TerrainMacroSample macro = sampleMacro(n, xz, s);
  return computeBaseHeight(macro, n, xz, s, edits);
}

// How many points around a candidate cell are probed for flatness. The lake
// takes the LOWEST of them as its surface, so a cell straddling a slope is
// both rejected (too much spread) and, if it survives, anchored low enough to
// stay inside its bowl.
constexpr int kLakeFlatnessProbes = 4;

struct CellData {
  bool isLake = false;
  glm::vec2 centre{0.0f};
  float radius = 0.0f;
  float surfaceY = kNoWater;
  float floorY = 0.0f;
};

CellData evaluateCell(const TerrainNoiseSet &n, const TerrainSettings &s, int32_t cx,
                      int32_t cz, const HeightOffsetGrid *edits) {
  CellData cell;
  const float cellSize = std::max(s.lakeScale, 8.0f);
  const uint32_t h = hashCell(cx, cz, s.seed ^ 0x5CA1AB1Eu);

  // Coverage gate first: most cells are not lakes, and bailing here skips
  // every height evaluation below.
  if (hashUnit(h) > saturate(s.lakeCoverage))
    return cell;

  // Jitter the centre inside its cell so the field does not read as a grid.
  const float jx = hashUnit(hashCell(cx, cz, s.seed ^ 0xA53Cu)) - 0.5f;
  const float jz = hashUnit(hashCell(cx, cz, s.seed ^ 0x7E1Du)) - 0.5f;
  cell.centre = glm::vec2((static_cast<float>(cx) + 0.5f + jx * 0.6f) * cellSize,
                          (static_cast<float>(cz) + 0.5f + jz * 0.6f) * cellSize);
  // Radius stays well under half a cell so two neighbouring lakes never
  // overlap -- which is what lets the nearest-cell lookup below be exact
  // rather than a blend of competing surfaces. Kept SMALL relative to the
  // cell for a second reason: a lake only survives the flatness test below if
  // the ground across it is level, and this generator's terrain rarely holds
  // still for 150 m. At the 0.22-0.42 fraction first tried, every candidate
  // on a 420 m grid was rejected and no lake ever appeared anywhere.
  const float radiusFrac = 0.09f + hashUnit(hashCell(cx, cz, s.seed ^ 0x1234u)) * 0.09f;
  cell.radius = cellSize * radiusFrac;

  // Valley bias. Lakes belong in the bottoms of things, and the landform
  // pipeline already computes exactly that signal (macroValley, 1 at a valley
  // floor). Using it makes the placement read as drainage rather than as
  // discs dropped on a grid, and it raises the hit rate of the flatness test
  // below because valley floors are the flat parts.
  const TerrainMacroSample centreMacro = sampleMacro(n, cell.centre, s);
  const float valley = saturate(centreMacro.macroValley);
  if (hashUnit(hashCell(cx, cz, s.seed ^ 0xBEEFu)) > 0.15f + valley * 0.85f)
    return cell;

  // Flatness test + surface anchor.
  float lowest = computeBaseHeight(centreMacro, n, cell.centre, s, edits);
  float highest = lowest;
  for (int i = 0; i < kLakeFlatnessProbes; ++i) {
    const float a = 6.2831853f * (static_cast<float>(i) / kLakeFlatnessProbes);
    const glm::vec2 p =
        cell.centre + glm::vec2(std::cos(a), std::sin(a)) * (cell.radius * 0.75f);
    const float hp = baseHeightAt(n, s, p, edits);
    lowest = std::min(lowest, hp);
    highest = std::max(highest, hp);
  }
  // A cell whose ground varies by far more than the lake could ever be deep
  // is a hillside, not a basin. Without some version of this, lakes appear as
  // flat discs pasted across slopes -- the worst artifact this approach can
  // produce. The threshold is generous (3x rather than the 1.35x first tried)
  // because the surface is anchored to the LOWEST probe below, which already
  // guarantees every measured rim point stands above the water; this test
  // only has to reject the genuinely absurd cases. At 1.35x almost no cell on
  // this generator's terrain qualified and no lake ever appeared.
  const float depth = std::max(s.lakeDepth, 0.1f);
  if (highest - lowest > depth * 3.0f)
    return cell;

  cell.isLake = true;
  // Anchor to the lowest probe, minus a little, so the rim is above water on
  // every side that was measured.
  cell.surfaceY = lowest - depth * 0.15f;
  cell.floorY = cell.surfaceY - depth;
  return cell;
}

const CellData *cachedCell(const TerrainNoiseSet &n, const TerrainSettings &s, int32_t cx,
                           int32_t cz, const HeightOffsetGrid *edits,
                           WaterCellCache *cache, CellData &scratch) {
  const int64_t key =
      (static_cast<int64_t>(cx) << 32) ^ (static_cast<uint32_t>(cz) & 0xFFFFFFFFu);
  if (cache) {
    for (int i = 0; i < cache->count; ++i) {
      if (cache->keys[i] == key) {
        scratch.isLake = cache->isLake[i];
        scratch.centre = cache->centre[i];
        scratch.radius = cache->radius[i];
        scratch.surfaceY = cache->surfaceY[i];
        scratch.floorY = cache->floorY[i];
        return &scratch;
      }
    }
  }
  scratch = evaluateCell(n, s, cx, cz, edits);
  if (cache && cache->count < WaterCellCache::kCapacity) {
    const int i = cache->count++;
    cache->keys[i] = key;
    cache->isLake[i] = scratch.isLake;
    cache->centre[i] = scratch.centre;
    cache->radius[i] = scratch.radius;
    cache->surfaceY[i] = scratch.surfaceY;
    cache->floorY[i] = scratch.floorY;
  }
  return &scratch;
}

} // namespace

float resolveSeaLevel(const TerrainNoiseSet &n, const TerrainSettings &s) {
  if (!s.oceanEnabled)
    return -1.0e9f;
  if (!s.autoSeaLevel)
    return s.seaLevel;
  // A bounded world's continent mask is BUILT around zero -- its 0.5 contour
  // is the shoreline by construction, and landCoverage is what decides how
  // much land there is. Deriving a percentile here as well would be a second
  // control fighting the first, and would drift the coast off the contour the
  // mask carved. Pin it.
  if (s.worldBounded)
    return 0.0f;

  // Sample base heights over an area much wider than the streamed disc, so
  // the percentile describes the world rather than whatever happens to be
  // near the origin. 96x96 is one-time work at terrain creation.
  constexpr int kGrid = 96;
  const float span = std::max(s.chunkWorldSize * s.viewDistanceChunks * 6.0f, 2000.0f);
  std::vector<float> heights;
  heights.reserve(kGrid * kGrid);
  for (int j = 0; j < kGrid; ++j) {
    for (int i = 0; i < kGrid; ++i) {
      const glm::vec2 p((static_cast<float>(i) / (kGrid - 1) - 0.5f) * span,
                        (static_cast<float>(j) / (kGrid - 1) - 0.5f) * span);
      heights.push_back(baseHeightAt(n, s, p, nullptr));
    }
  }
  const float coverage = saturate(s.oceanCoverage);
  size_t idx = static_cast<size_t>(coverage * (heights.size() - 1));
  std::nth_element(heights.begin(), heights.begin() + idx, heights.end());
  return heights[idx];
}

LakeSample sampleLake(const TerrainNoiseSet &n, const TerrainSettings &s, glm::vec2 worldXZ,
                      const HeightOffsetGrid *edits, WaterCellCache *cache) {
  LakeSample out;
  if (!s.lakesEnabled || s.lakeCoverage <= 0.0f)
    return out;

  const float cellSize = std::max(s.lakeScale, 8.0f);
  const int32_t cx = static_cast<int32_t>(std::floor(worldXZ.x / cellSize));
  const int32_t cz = static_cast<int32_t>(std::floor(worldXZ.y / cellSize));

  // Radii are capped below half a cell, so a point can only be inside its own
  // cell's lake or one of the 8 neighbours'. Checking all 9 costs nothing but
  // hashes for the (overwhelmingly common) non-lake cells.
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      CellData scratch;
      const CellData *cell = cachedCell(n, s, cx + dx, cz + dz, edits, cache, scratch);
      if (!cell->isLake)
        continue;
      const float dist = glm::length(worldXZ - cell->centre);
      if (dist >= cell->radius)
        continue;
      // Smooth bowl: 1 at the centre, 0 at the rim. Squared so the floor is
      // broad and flat and the walls steepen toward the shore, which is what
      // a real basin cross-section looks like.
      const float t = 1.0f - dist / cell->radius;
      out.inLake = true;
      out.surfaceY = cell->surfaceY;
      out.floorY = cell->floorY;
      out.basin = t * t * (3.0f - 2.0f * t);
      return out;
    }
  }
  return out;
}

float waterSurfaceAt(const TerrainNoiseSet &n, const TerrainSettings &s, glm::vec2 worldXZ,
                     const HeightOffsetGrid *edits, WaterCellCache *cache) {
  float surface = s.oceanEnabled ? s.seaLevel : kNoWater;
  const LakeSample lake = sampleLake(n, s, worldXZ, edits, cache);
  if (lake.inLake)
    surface = std::max(surface, lake.surfaceY);
  return surface;
}

float applyWaterToHeight(float h, const LakeSample &lake, const TerrainSettings &s) {
  if (lake.inLake) {
    // Only ever LOWER the ground. Blending toward the floor unconditionally
    // would raise terrain that already sits below the basin floor, filling in
    // a gorge that happened to pass under the lake.
    h = h * (1.0f - lake.basin) + std::min(h, lake.floorY) * lake.basin;
  }

  if (s.oceanEnabled && s.shoreFlatten > 0.0f) {
    // Coastlines. Compressing relief within a band around the sea line turns
    // what would otherwise be an arbitrary contour across ordinary hillside --
    // cliffs everywhere the slope happens to be steep -- into shallows and
    // beaches. The sign is preserved, so this never moves land across the
    // waterline, only changes how fast it gets there.
    const float band = std::max(s.shoreFlatten, 0.01f);
    const float d = h - s.seaLevel;
    const float a = std::fabs(d);
    if (a < band) {
      const float t = a / band;
      h = s.seaLevel + (d < 0.0f ? -1.0f : 1.0f) * band * (t * t) * (1.0f - 0.35f * (1.0f - t));
    }
  }
  return h;
}

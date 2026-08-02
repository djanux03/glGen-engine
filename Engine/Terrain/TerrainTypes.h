#pragma once
// TerrainTypes.h — shared plain-data types for the terrain system.
// ChunkCoord/ChunkState are added in Phase 2 (TerrainChunkManager);
// VegSpecies is added in Phase 3 (TerrainVegetation).
//
// R1 (MEADOW_TERRAIN_REVAMP_PLAN.md): the old 6-way BiomeType
// (Ocean/Plains/Forest/Desert/Mountains/Tundra) is deleted with no
// successor. In its place, every world position carries a continuous
// BiomeWeights triple (meadow/forest/mountain, summing to ~1) instead of a
// single classification -- every consumer (material, scatter, lighting)
// blends by these weights so biome borders are gradients, never lines.
// Ocean/Desert/Tundra have no replacement at all (this engine is a single
// green world now); old Forest/Mountains are redesigned from scratch in
// Engine/Terrain/TerrainNoise.cpp, not ported.

#include <cmath>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>

// Continuous per-point biome membership, always summing to ~1.0. Computed
// once per height/ground-field sample (TerrainNoise.cpp's
// sampleBiomeWeights()) and threaded through to material splatting, scatter
// density gating, and per-pixel/per-instance lighting -- never thresholded
// into a hard category (that would reintroduce a visible biome border).
struct BiomeWeights {
  float meadow = 1.0f;
  float forest = 0.0f;
  float mountain = 0.0f;
};

// Integer chunk-grid coordinate: worldXZ / chunkWorldSize, floored.
struct ChunkCoord {
  int32_t x = 0;
  int32_t z = 0;

  bool operator==(const ChunkCoord &other) const {
    return x == other.x && z == other.z;
  }
  bool operator!=(const ChunkCoord &other) const { return !(*this == other); }
};

enum class ChunkState {
  Unloaded,
  Queued,
  Building,
  ReadyToUpload,
  Active,
  Unloading
};

// Chunk (x,z) occupies the world-space cell [x*size, (x+1)*size) x
// [z*size, (z+1)*size) -- a standard corner-indexed grid. These two helpers
// are the single source of truth for that convention, shared by
// TerrainChunkManager (which samples heights at the min corner, matching
// sampleHeightGrid's `chunkOrigin` = min-corner contract) and the renderer
// glue (which places the chunk-local mesh -- centered at its own origin --
// via a translate-to-center instance transform). Getting these out of sync
// would misalign height sampling with mesh placement and break seams.
inline ChunkCoord chunkCoordFromWorldXZ(glm::vec2 worldXZ, float chunkWorldSize) {
  return ChunkCoord{static_cast<int32_t>(std::floor(worldXZ.x / chunkWorldSize)),
                    static_cast<int32_t>(std::floor(worldXZ.y / chunkWorldSize))};
}
inline glm::vec2 chunkMinCorner(ChunkCoord coord, float chunkWorldSize) {
  return glm::vec2(static_cast<float>(coord.x), static_cast<float>(coord.z)) *
         chunkWorldSize;
}
inline glm::vec2 chunkCenterWorld(ChunkCoord coord, float chunkWorldSize) {
  return chunkMinCorner(coord, chunkWorldSize) +
         glm::vec2(chunkWorldSize * 0.5f);
}

namespace std {
template <> struct hash<ChunkCoord> {
  size_t operator()(const ChunkCoord &c) const noexcept {
    // Combine two 32-bit ints into a 64-bit key, then hash that.
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(c.x)) << 32) |
                   static_cast<uint32_t>(c.z);
    return hash<uint64_t>()(key);
  }
};
} // namespace std

// Deterministic per-chunk seed for vegetation placement: same (settings.seed,
// coord) always scatters identically, so vegetation doesn't need to persist
// across unload/reload (only brush edits, Phase 5, need persistence).
inline uint32_t chunkSeedFor(uint32_t seed, ChunkCoord coord) {
  const uint64_t combined =
      (static_cast<uint64_t>(seed) << 32) ^ std::hash<ChunkCoord>()(coord);
  return static_cast<uint32_t>(combined ^ (combined >> 32));
}

// R4 (MEADOW_TERRAIN_REVAMP_PLAN.md §6): the old fixed VegSpecies enum +
// VegetationInstance (Phase 3 of the pre-revamp plan) are deleted. Species
// are no longer a closed enum of procedurally-built meshes -- they're
// data-driven layers in a ScatterManifest (Engine/Terrain/ScatterManifest.h),
// each pointing at a real .obj/.fbx/.gltf file. See
// Engine/Terrain/TerrainScatter.h for the instance type that replaces
// VegetationInstance (ScatterInstance).

#pragma once
// HeightOffsetGrid.h — Phase 5: sparse per-chunk height-edit storage for the
// terrain brush. computeHeight() composites this as the top layer over
// procedural noise (see TerrainNoise.cpp); TerrainChunkManager owns the one
// instance shared by worker jobs (read-only) and the main-thread brush tool
// (the only writer).

#include "TerrainTypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <shared_mutex>
#include <memory>
#include <unordered_map>
#include <vector>

class HeightOffsetGrid {
public:
  // Applies a radial linear-falloff height delta centered at worldXZ,
  // touching every chunk whose footprint intersects the brush radius.
  // Lazily allocates each touched chunk's grid (chunkResolution^2 floats,
  // zero-initialized, same row-major layout/point positions
  // sampleHeightGrid() uses) on first touch. Main-thread only.
  void applyBrush(glm::vec2 worldXZ, float radius, float strength,
                  float chunkWorldSize, uint32_t chunkResolution);

  // Bilinearly samples the offset at worldXZ; 0 where nothing has been
  // painted (including chunks with no grid allocated yet). Safe to call
  // concurrently from worker threads while applyBrush() runs on the main
  // thread (shared/read lock here, unique/write lock there).
  float sampleBilinear(glm::vec2 worldXZ, float chunkWorldSize,
                       uint32_t chunkResolution) const;

  // Returns and clears the set of chunk coords touched by applyBrush()
  // calls since the last takeTouchedChunks() call. Main-thread only.
  std::vector<ChunkCoord> takeTouchedChunks();

  // Empties every stored grid (e.g. on a full terrain regenerate -- old
  // edits are geometrically desynced or artistically meaningless against a
  // newly-generated terrain shape). Main-thread only.
  void clear();
  // Worker jobs keep a private immutable copy, so a later edit/regeneration
  // cannot change a fog field halfway through its evaluation.
  std::shared_ptr<const HeightOffsetGrid> snapshot() const;

private:
  std::vector<float> &gridFor(ChunkCoord coord, uint32_t chunkResolution);

  mutable std::shared_mutex mMutex;
  std::unordered_map<ChunkCoord, std::vector<float>> mGrids;
  std::vector<ChunkCoord> mTouched;
};

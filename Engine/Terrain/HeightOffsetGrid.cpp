#include "HeightOffsetGrid.h"

#include <algorithm>
#include <cmath>

std::shared_ptr<const HeightOffsetGrid> HeightOffsetGrid::snapshot() const {
  auto copy = std::make_shared<HeightOffsetGrid>();
  std::shared_lock<std::shared_mutex> lock(mMutex);
  copy->mGrids = mGrids;
  return copy;
}

std::vector<float> &HeightOffsetGrid::gridFor(ChunkCoord coord,
                                              uint32_t chunkResolution) {
  auto it = mGrids.find(coord);
  if (it != mGrids.end())
    return it->second;
  auto inserted = mGrids.emplace(
      coord, std::vector<float>(static_cast<size_t>(chunkResolution) *
                                chunkResolution, 0.0f));
  return inserted.first->second;
}

void HeightOffsetGrid::applyBrush(glm::vec2 worldXZ, float radius,
                                  float strength, float chunkWorldSize,
                                  uint32_t chunkResolution) {
  if (radius <= 0.0f || chunkResolution < 2)
    return;

  const ChunkCoord minChunk = chunkCoordFromWorldXZ(
      worldXZ - glm::vec2(radius), chunkWorldSize);
  const ChunkCoord maxChunk = chunkCoordFromWorldXZ(
      worldXZ + glm::vec2(radius), chunkWorldSize);

  std::unique_lock<std::shared_mutex> lock(mMutex);
  const float denom = static_cast<float>(chunkResolution - 1);

  for (int32_t cz = minChunk.z; cz <= maxChunk.z; ++cz) {
    for (int32_t cx = minChunk.x; cx <= maxChunk.x; ++cx) {
      const ChunkCoord coord{cx, cz};
      const glm::vec2 minCorner = chunkMinCorner(coord, chunkWorldSize);
      std::vector<float> &grid = gridFor(coord, chunkResolution);

      bool touched = false;
      for (uint32_t j = 0; j < chunkResolution; ++j) {
        for (uint32_t i = 0; i < chunkResolution; ++i) {
          const glm::vec2 pos =
              minCorner + glm::vec2(static_cast<float>(i) / denom,
                                    static_cast<float>(j) / denom) *
                              chunkWorldSize;
          const float dist = glm::length(pos - worldXZ);
          if (dist > radius)
            continue;
          const float falloff = 1.0f - dist / radius;
          grid[static_cast<size_t>(j) * chunkResolution + i] +=
              strength * falloff;
          touched = true;
        }
      }
      if (touched)
        mTouched.push_back(coord);
    }
  }
}

float HeightOffsetGrid::sampleBilinear(glm::vec2 worldXZ, float chunkWorldSize,
                                       uint32_t chunkResolution) const {
  if (chunkResolution < 2)
    return 0.0f;

  const ChunkCoord coord = chunkCoordFromWorldXZ(worldXZ, chunkWorldSize);

  std::shared_lock<std::shared_mutex> lock(mMutex);
  auto it = mGrids.find(coord);
  if (it == mGrids.end())
    return 0.0f;
  const std::vector<float> &grid = it->second;

  const glm::vec2 minCorner = chunkMinCorner(coord, chunkWorldSize);
  const float denom = static_cast<float>(chunkResolution - 1);
  const glm::vec2 local = (worldXZ - minCorner) / chunkWorldSize * denom;

  const int i0 = std::clamp(static_cast<int>(std::floor(local.x)), 0,
                            static_cast<int>(chunkResolution) - 1);
  const int j0 = std::clamp(static_cast<int>(std::floor(local.y)), 0,
                            static_cast<int>(chunkResolution) - 1);
  const int i1 = std::min(i0 + 1, static_cast<int>(chunkResolution) - 1);
  const int j1 = std::min(j0 + 1, static_cast<int>(chunkResolution) - 1);
  const float tx = std::clamp(local.x - static_cast<float>(i0), 0.0f, 1.0f);
  const float ty = std::clamp(local.y - static_cast<float>(j0), 0.0f, 1.0f);

  auto at = [&](int i, int j) {
    return grid[static_cast<size_t>(j) * chunkResolution + i];
  };
  const float v00 = at(i0, j0), v10 = at(i1, j0);
  const float v01 = at(i0, j1), v11 = at(i1, j1);
  const float v0 = v00 + (v10 - v00) * tx;
  const float v1 = v01 + (v11 - v01) * tx;
  return v0 + (v1 - v0) * ty;
}

std::vector<ChunkCoord> HeightOffsetGrid::takeTouchedChunks() {
  std::unique_lock<std::shared_mutex> lock(mMutex);
  std::vector<ChunkCoord> out;
  out.swap(mTouched);
  return out;
}

void HeightOffsetGrid::clear() {
  std::unique_lock<std::shared_mutex> lock(mMutex);
  mGrids.clear();
  mTouched.clear();
}

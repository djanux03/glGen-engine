#include <doctest/doctest.h>
#include "TerrainChunkManager.h"
#include "TerrainChunkMesher.h"
#include "TerrainNoise.h"
#include "TerrainTypes.h"

#include <algorithm>
#include <cmath>
#include <set>

TEST_CASE("TerrainChunkManager — sampleCollisionHeights always returns chunkResolution^2 samples") {
  TerrainSettings settings;
  settings.workerThreads = 1;
  TerrainChunkManager manager;
  manager.init(settings);

  // Same fixed LOD0 resolution regardless of what LOD a chunk at that coord
  // would use visually -- collision doesn't take a lod parameter at all.
  std::vector<float> heights;
  manager.sampleCollisionHeights(ChunkCoord{0, 0}, heights);
  CHECK(heights.size() == static_cast<size_t>(settings.chunkResolution) *
                              settings.chunkResolution);

  manager.sampleCollisionHeights(ChunkCoord{7, -4}, heights);
  CHECK(heights.size() == static_cast<size_t>(settings.chunkResolution) *
                              settings.chunkResolution);

  for (float h : heights)
    CHECK(std::isfinite(h));

  manager.shutdown();
}

TEST_CASE("TerrainChunkManager — sampleCollisionHeights matches sampleHeightGrid at LOD0") {
  TerrainSettings settings;
  settings.workerThreads = 1;
  TerrainChunkManager manager;
  manager.init(settings);

  const ChunkCoord coord{2, -1};
  std::vector<float> viaManager;
  manager.sampleCollisionHeights(coord, viaManager);

  TerrainNoiseSet noise(settings.seed);
  std::vector<float> viaDirect;
  sampleHeightGrid(noise, settings, chunkMinCorner(coord, settings.chunkWorldSize),
                   settings.chunkWorldSize, settings.chunkResolution, nullptr,
                   viaDirect, nullptr);

  REQUIRE(viaManager.size() == viaDirect.size());
  for (size_t i = 0; i < viaManager.size(); ++i)
    CHECK(viaManager[i] == doctest::Approx(viaDirect[i]));

  manager.shutdown();
}

TEST_CASE("TerrainChunkManager — desiredChunkSet covers a Chebyshev disc, nearest-first") {
  const ChunkCoord center{5, -3};
  const int radius = 2;
  auto jobs = TerrainChunkManager::desiredChunkSet(center, radius);

  CHECK(jobs.size() == static_cast<size_t>((2 * radius + 1) * (2 * radius + 1)));

  // Every coord within the Chebyshev disc appears exactly once.
  std::set<std::pair<int32_t, int32_t>> seen;
  for (const auto &j : jobs) {
    const int dx = std::abs(j.coord.x - center.x);
    const int dz = std::abs(j.coord.z - center.z);
    CHECK(std::max(dx, dz) <= radius);
    seen.insert({j.coord.x, j.coord.z});
  }
  CHECK(seen.size() == jobs.size());

  // Nearest-first: squared distance to center is non-decreasing.
  int64_t prevDist = -1;
  for (const auto &j : jobs) {
    const int64_t dx = j.coord.x - center.x;
    const int64_t dz = j.coord.z - center.z;
    const int64_t dist = dx * dx + dz * dz;
    CHECK(dist >= prevDist);
    prevDist = dist;
  }

  // The center chunk itself is first and gets the finest LOD.
  CHECK(jobs.front().coord == center);
  CHECK(jobs.front().lod == 0);
}

TEST_CASE("TerrainChunkManager — lodForDistance is finest at 0 and coarsens monotonically") {
  const int viewDistance = 6;
  CHECK(TerrainChunkManager::lodForDistance(0, viewDistance) == 0);
  CHECK(TerrainChunkManager::lodForDistance(viewDistance, viewDistance) == 4);

  int prevLod = 0;
  for (int d = 0; d <= viewDistance; ++d) {
    const int lod = TerrainChunkManager::lodForDistance(d, viewDistance);
    CHECK(lod >= prevLod);
    CHECK(lod >= 0);
    CHECK(lod <= 4);
    prevLod = lod;
  }
}

TEST_CASE("TerrainChunkManager — lodSamplesPerEdge matches the 33/17/9/5/3 table") {
  CHECK(TerrainChunkManager::lodSamplesPerEdge(33, 0) == 33);
  CHECK(TerrainChunkManager::lodSamplesPerEdge(33, 1) == 17);
  CHECK(TerrainChunkManager::lodSamplesPerEdge(33, 2) == 9);
  CHECK(TerrainChunkManager::lodSamplesPerEdge(33, 3) == 5);
  CHECK(TerrainChunkManager::lodSamplesPerEdge(33, 4) == 3);
}

TEST_CASE("TerrainTypes — chunk coord/origin/center helpers agree with each other") {
  const float size = 64.0f;
  const ChunkCoord coord{2, -1};

  const glm::vec2 minCorner = chunkMinCorner(coord, size);
  const glm::vec2 center = chunkCenterWorld(coord, size);
  CHECK(minCorner.x == doctest::Approx(128.0f));
  CHECK(minCorner.y == doctest::Approx(-64.0f));
  CHECK(center.x == doctest::Approx(160.0f));
  CHECK(center.y == doctest::Approx(-32.0f));

  // A world position strictly inside the cell round-trips to the same coord.
  const glm::vec2 insideCell = center;
  CHECK(chunkCoordFromWorldXZ(insideCell, size) == coord);

  // The min corner itself belongs to this cell (half-open interval).
  CHECK(chunkCoordFromWorldXZ(minCorner, size) == coord);
}

TEST_CASE("TerrainChunkMesher — skirts add a closed ring without disturbing the grid") {
  const uint32_t samplesPerEdge = 5;
  const float chunkWorldSize = 32.0f;
  std::vector<float> heights(samplesPerEdge * samplesPerEdge, 3.0f);
  std::vector<TerrainGroundFields> fields(heights.size());

  MeshData noSkirt = buildTerrainChunkMesh(heights, fields, samplesPerEdge,
                                           chunkWorldSize, 0.55f, 16.0f, 0.0f);
  MeshData withSkirt = buildTerrainChunkMesh(heights, fields, samplesPerEdge,
                                             chunkWorldSize, 0.55f, 16.0f, 4.0f);

  REQUIRE(noSkirt.submeshes.size() == 1);
  REQUIRE(withSkirt.submeshes.size() == 1);

  const auto &flat = noSkirt.submeshes[0];
  const auto &skirted = withSkirt.submeshes[0];

  const size_t gridVerts = static_cast<size_t>(samplesPerEdge) * samplesPerEdge;
  const size_t ringLen = 4 * (samplesPerEdge - 1);

  CHECK(flat.vertices.size() == gridVerts);
  CHECK(skirted.vertices.size() == gridVerts + ringLen);

  // The first gridVerts vertices are identical between the two meshes (skirt
  // generation must not perturb the original grid).
  for (size_t i = 0; i < gridVerts; ++i) {
    CHECK(skirted.vertices[i].pos.x == doctest::Approx(flat.vertices[i].pos.x));
    CHECK(skirted.vertices[i].pos.y == doctest::Approx(flat.vertices[i].pos.y));
    CHECK(skirted.vertices[i].pos.z == doctest::Approx(flat.vertices[i].pos.z));
  }

  // Every skirt vertex is exactly skirtDepth below its ring source vertex.
  for (size_t i = gridVerts; i < skirted.vertices.size(); ++i)
    CHECK(skirted.vertices[i].pos.y == doctest::Approx(3.0f - 4.0f));

  // Skirt adds exactly 2 triangles (6 indices) per ring edge, closed loop.
  CHECK(skirted.indices.size() == flat.indices.size() + ringLen * 6);
}

TEST_CASE("TerrainChunkMesher — zero skirtDepth disables skirt geometry") {
  const uint32_t samplesPerEdge = 3;
  std::vector<float> heights(samplesPerEdge * samplesPerEdge, 0.0f);
  MeshData data = buildTerrainChunkMesh(heights, {}, samplesPerEdge, 16.0f,
                                        0.55f, 16.0f, /*skirtDepth=*/0.0f);
  REQUIRE(data.submeshes.size() == 1);
  CHECK(data.submeshes[0].vertices.size() ==
        static_cast<size_t>(samplesPerEdge) * samplesPerEdge);
}

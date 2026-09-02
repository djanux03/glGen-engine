#include <doctest/doctest.h>
#include "HeightOffsetGrid.h"
#include "TerrainNoise.h"
#include "TerrainQuery.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

TEST_CASE("HeightOffsetGrid — applyBrush/sampleBilinear round-trip") {
  HeightOffsetGrid grid;
  const float chunkWorldSize = 64.0f;
  const uint32_t chunkResolution = 33;
  const glm::vec2 center(10.0f, 10.0f);
  const float radius = 8.0f;
  const float strength = 5.0f;

  CHECK(grid.sampleBilinear(center, chunkWorldSize, chunkResolution) ==
        doctest::Approx(0.0f));

  grid.applyBrush(center, radius, strength, chunkWorldSize, chunkResolution);

  // At the center, full strength (falloff = 1 - 0/radius = 1).
  const float atCenter =
      grid.sampleBilinear(center, chunkWorldSize, chunkResolution);
  CHECK(atCenter == doctest::Approx(strength).epsilon(0.15));

  // Well outside the radius, still ~0.
  const float farAway = grid.sampleBilinear(
      center + glm::vec2(radius * 4.0f, 0.0f), chunkWorldSize, chunkResolution);
  CHECK(farAway == doctest::Approx(0.0f));

  // A chunk this brush never touched stays at exactly 0.
  const float untouched = grid.sampleBilinear(
      glm::vec2(10000.0f, 10000.0f), chunkWorldSize, chunkResolution);
  CHECK(untouched == doctest::Approx(0.0f));
}

TEST_CASE("HeightOffsetGrid — takeTouchedChunks reports and clears touched coords") {
  HeightOffsetGrid grid;
  const float chunkWorldSize = 64.0f;
  const uint32_t chunkResolution = 33;

  CHECK(grid.takeTouchedChunks().empty());

  grid.applyBrush(glm::vec2(5.0f, 5.0f), 3.0f, 1.0f, chunkWorldSize,
                  chunkResolution);
  auto touched = grid.takeTouchedChunks();
  CHECK(!touched.empty());
  CHECK(std::find(touched.begin(), touched.end(), ChunkCoord{0, 0}) !=
        touched.end());

  // Draining again with no new edits returns nothing.
  CHECK(grid.takeTouchedChunks().empty());
}

TEST_CASE("computeHeight — reflects a non-null edits grid's offset") {
  TerrainSettings settings;
  settings.heightScale = 0.0f; // flat procedural terrain -> isolates the edit
  // worldBounded shapes land toward landBaseHeight and sea toward
  // -oceanFloorDepth in ABSOLUTE metres relative to sea level, so it is
  // deliberately independent of heightScale: zeroing the relief amplitude
  // no longer implies a flat world once the world has a shape. This test
  // wants a synthetic flat field as a FIXTURE, so it opts out.
  settings.worldBounded = false;
  TerrainNoiseSet noiseSet(7);

  HeightOffsetGrid edits;
  const glm::vec2 p(20.0f, -20.0f);
  edits.applyBrush(p, 5.0f, 3.0f, settings.chunkWorldSize,
                   settings.chunkResolution);

  TerrainMacroSample sample = sampleMacro(noiseSet, p, settings);
  const float hNoEdits = computeHeight(sample, noiseSet, p, settings, nullptr);
  const float hWithEdits = computeHeight(sample, noiseSet, p, settings, &edits);

  CHECK(hNoEdits == doctest::Approx(0.0f));
  CHECK(hWithEdits > hNoEdits);
}

TEST_CASE("TerrainQuery::raycast — hits a flat plane at the expected distance") {
  TerrainSettings settings;
  settings.heightScale = 0.0f; // flat terrain at height 0
  // worldBounded shapes land toward landBaseHeight and sea toward
  // -oceanFloorDepth in ABSOLUTE metres relative to sea level, so it is
  // deliberately independent of heightScale: zeroing the relief amplitude
  // no longer implies a flat world once the world has a shape. This test
  // wants a synthetic flat field as a FIXTURE, so it opts out.
  settings.worldBounded = false;
  TerrainNoiseSet noiseSet(11);
  TerrainQuery query(settings, noiseSet);

  const glm::vec3 origin(0.0f, 10.0f, 0.0f);
  const glm::vec3 dir(0.0f, -1.0f, 0.0f);
  auto hit = query.raycast(origin, dir, /*maxDistance=*/100.0f, /*stepSize=*/0.5f);

  REQUIRE(hit.hit);
  CHECK(hit.distance == doctest::Approx(10.0f).epsilon(0.05));
  CHECK(hit.point.y == doctest::Approx(0.0f).epsilon(0.05));
  CHECK(hit.xz.x == doctest::Approx(0.0f));
  CHECK(hit.xz.y == doctest::Approx(0.0f));
}

TEST_CASE("TerrainQuery::raycast — misses when aimed away from terrain") {
  TerrainSettings settings;
  settings.heightScale = 0.0f;
  TerrainNoiseSet noiseSet(11);
  TerrainQuery query(settings, noiseSet);

  const glm::vec3 origin(0.0f, 10.0f, 0.0f);
  const glm::vec3 dir(0.0f, 1.0f, 0.0f); // straight up, away from the ground
  auto hit = query.raycast(origin, dir, 100.0f, 0.5f);
  CHECK_FALSE(hit.hit);
}

TEST_CASE("TerrainQuery::raycast — a ray already underground reports no hit") {
  TerrainSettings settings;
  settings.heightScale = 0.0f;
  TerrainNoiseSet noiseSet(11);
  TerrainQuery query(settings, noiseSet);

  const glm::vec3 origin(0.0f, -5.0f, 0.0f);
  const glm::vec3 dir(0.0f, -1.0f, 0.0f);
  auto hit = query.raycast(origin, dir, 100.0f, 0.5f);
  CHECK_FALSE(hit.hit);
}

#include <doctest/doctest.h>
#include "TerrainNoise.h"
#include "TerrainQuery.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <set>

TEST_CASE("TerrainNoise — height deterministic with same seed") {
  TerrainSettings settings;
  TerrainNoiseSet a(1337);
  TerrainNoiseSet b(1337);

  for (int i = 0; i < 20; ++i) {
    glm::vec2 p(i * 17.3f, i * -11.9f);
    TerrainMacroSample sa = sampleMacro(a, p, settings);
    TerrainMacroSample sb = sampleMacro(b, p, settings);
    float ha = computeHeight(sa, a, p, settings, nullptr);
    float hb = computeHeight(sb, b, p, settings, nullptr);
    CHECK(ha == doctest::Approx(hb));
  }
}

TEST_CASE("TerrainNoise — different seeds produce different heights") {
  TerrainSettings settings;
  TerrainNoiseSet a(1), b(2);
  int diffs = 0;

  for (int i = 0; i < 50; ++i) {
    glm::vec2 p(i * 13.1f, i * 7.7f);
    TerrainMacroSample sa = sampleMacro(a, p, settings);
    TerrainMacroSample sb = sampleMacro(b, p, settings);
    float ha = computeHeight(sa, a, p, settings, nullptr);
    float hb = computeHeight(sb, b, p, settings, nullptr);
    if (std::abs(ha - hb) > 1e-4f) diffs++;
  }
  CHECK(diffs > 25);
}

TEST_CASE("TerrainNoise — height stays within a sane bound relative to heightScale") {
  TerrainSettings settings;
  TerrainNoiseSet noiseSet(42);
  // Generous, catches blow-ups/NaNs, not tuned to exact math. R1's mountain
  // relief legitimately reaches ~macroMountain*1.6*mountainHeightScale +
  // macroRidge*0.6*mountainHeightScale (+ the base shape term) at default
  // settings -- with mountainHeightScale=3.2 that's already ~140 at
  // heightScale=18, so the multiplier is scaled by mountainHeightScale
  // rather than left as a flat constant that mountain tuning could outrun.
  const float bound = settings.heightScale * (8.0f + settings.mountainHeightScale * 3.0f);

  for (int i = 0; i < 200; ++i) {
    glm::vec2 p((i * 31.7f) - 3000.0f, (i * 19.3f) - 3000.0f);
    TerrainMacroSample s = sampleMacro(noiseSet, p, settings);
    float h = computeHeight(s, noiseSet, p, settings, nullptr);
    CHECK(std::isfinite(h));
    CHECK(h > -bound);
    CHECK(h < bound);
  }
}

TEST_CASE("TerrainNoise — biome weights always sum to ~1 and stay in [0,1]") {
  TerrainSettings settings;
  TerrainNoiseSet noiseSet(7);

  for (int i = 0; i < 60; ++i) {
    for (int j = 0; j < 60; ++j) {
      glm::vec2 p((i * 25.0f) - 750.0f, (j * 25.0f) - 750.0f);
      TerrainMacroSample s = sampleMacro(noiseSet, p, settings);
      float h = computeHeight(s, noiseSet, p, settings, nullptr);
      BiomeWeights w = sampleBiomeWeights(s, settings, h);
      CHECK(w.meadow >= 0.0f);
      CHECK(w.forest >= 0.0f);
      CHECK(w.mountain >= 0.0f);
      CHECK(w.meadow <= 1.0f);
      CHECK(w.forest <= 1.0f);
      CHECK(w.mountain <= 1.0f);
      CHECK((w.meadow + w.forest + w.mountain) == doctest::Approx(1.0f).epsilon(0.01f));
    }
  }
}

TEST_CASE("TerrainNoise — a wide grid produces real biome variety (not stuck at pure meadow)") {
  TerrainSettings settings;
  TerrainNoiseSet noiseSet(7);

  float maxForest = 0.0f, maxMountain = 0.0f;
  for (int i = 0; i < 80; ++i) {
    for (int j = 0; j < 80; ++j) {
      glm::vec2 p((i * 40.0f) - 1600.0f, (j * 40.0f) - 1600.0f);
      TerrainMacroSample s = sampleMacro(noiseSet, p, settings);
      float h = computeHeight(s, noiseSet, p, settings, nullptr);
      BiomeWeights w = sampleBiomeWeights(s, settings, h);
      maxForest = std::max(maxForest, w.forest);
      maxMountain = std::max(maxMountain, w.mountain);
    }
  }
  CHECK(maxForest > 0.3f);
  CHECK(maxMountain > 0.3f);
}

TEST_CASE("TerrainNoise — high mountainCoverage raises the fraction of mostly-mountain samples") {
  TerrainNoiseSet noiseSet(11);
  TerrainSettings low, high;
  low.mountainCoverage = 0.05f;
  high.mountainCoverage = 0.9f;

  auto mostlyMountainFraction = [&](const TerrainSettings &settings) {
    int mountainCount = 0, total = 0;
    for (int i = 0; i < 40; ++i) {
      for (int j = 0; j < 40; ++j) {
        glm::vec2 p((i * 60.0f) - 1200.0f, (j * 60.0f) - 1200.0f);
        TerrainMacroSample s = sampleMacro(noiseSet, p, settings);
        float h = computeHeight(s, noiseSet, p, settings, nullptr);
        BiomeWeights w = sampleBiomeWeights(s, settings, h);
        if (w.mountain > 0.5f)
          ++mountainCount;
        ++total;
      }
    }
    return static_cast<float>(mountainCount) / static_cast<float>(total);
  };

  CHECK(mostlyMountainFraction(high) > mostlyMountainFraction(low));
}

TEST_CASE("TerrainNoise — sampleHeightGrid matches per-point computeHeight/sampleBiomeWeights at the same coordinates") {
  TerrainSettings settings;
  settings.chunkResolution = 9;
  TerrainNoiseSet noiseSet(99);

  glm::vec2 chunkOrigin(128.0f, -64.0f);
  float chunkWorldSize = settings.chunkWorldSize;
  uint32_t samplesPerEdge = settings.chunkResolution;

  std::vector<float> heights;
  std::vector<TerrainGroundFields> fields;
  sampleHeightGrid(noiseSet, settings, chunkOrigin, chunkWorldSize, samplesPerEdge, nullptr, heights, &fields);

  REQUIRE(heights.size() == samplesPerEdge * samplesPerEdge);
  REQUIRE(fields.size() == samplesPerEdge * samplesPerEdge);

  const float denom = static_cast<float>(samplesPerEdge - 1);
  for (uint32_t j = 0; j < samplesPerEdge; ++j) {
    for (uint32_t i = 0; i < samplesPerEdge; ++i) {
      glm::vec2 p = chunkOrigin + glm::vec2(i / denom, j / denom) * chunkWorldSize;
      TerrainMacroSample s = sampleMacro(noiseSet, p, settings);
      float expectedHeight = computeHeight(s, noiseSet, p, settings, nullptr);
      BiomeWeights expectedWeights = sampleBiomeWeights(s, settings, expectedHeight);

      uint32_t idx = j * samplesPerEdge + i;
      CHECK(heights[idx] == doctest::Approx(expectedHeight));
      CHECK(fields[idx].wMeadow == doctest::Approx(expectedWeights.meadow));
      CHECK(fields[idx].wForest == doctest::Approx(expectedWeights.forest));
      CHECK(fields[idx].wMountain == doctest::Approx(expectedWeights.mountain));
    }
  }
}

TEST_CASE("TerrainQuery — normalAt returns a roughly unit-length vector") {
  TerrainSettings settings;
  TerrainNoiseSet noiseSet(2024);
  TerrainQuery query(settings, noiseSet);

  for (int i = 0; i < 30; ++i) {
    glm::vec2 p(i * 19.0f, i * -23.0f);
    glm::vec3 n = query.normalAt(p);
    float len = glm::length(n);
    CHECK(len == doctest::Approx(1.0f).epsilon(0.02f));
  }
}

TEST_CASE("TerrainQuery — slopeAt is in [0,1] and finite on real terrain") {
  TerrainSettings settings;
  TerrainNoiseSet noiseSet(2024);
  TerrainQuery query(settings, noiseSet);

  for (int i = 0; i < 30; ++i) {
    glm::vec2 p(i * 19.0f, i * -23.0f);
    float slope = query.slopeAt(p);
    CHECK(std::isfinite(slope));
    CHECK(slope >= 0.0f);
    CHECK(slope <= 1.0f);
  }
}

TEST_CASE("TerrainQuery — slopeAt and normalAt on a synthetic flat field (heightScale=0)") {
  TerrainSettings settings;
  settings.heightScale = 0.0f;  // forces computeHeight() to 0 everywhere -> perfectly flat
  TerrainNoiseSet noiseSet(5);
  TerrainQuery query(settings, noiseSet);

  glm::vec2 p(123.0f, -45.0f);
  CHECK(query.heightAt(p) == doctest::Approx(0.0f));
  CHECK(query.slopeAt(p) == doctest::Approx(0.0f).epsilon(0.001f));

  glm::vec3 n = query.normalAt(p);
  CHECK(n.x == doctest::Approx(0.0f).epsilon(0.001f));
  CHECK(n.y == doctest::Approx(1.0f).epsilon(0.001f));
  CHECK(n.z == doctest::Approx(0.0f).epsilon(0.001f));
}

TEST_CASE("TerrainQuery — biomeWeightsAt matches sampleBiomeWeights at the same coordinates") {
  TerrainSettings settings;
  TerrainNoiseSet noiseSet(2024);
  TerrainQuery query(settings, noiseSet);

  for (int i = 0; i < 30; ++i) {
    glm::vec2 p(i * 33.0f, i * -17.0f);
    BiomeWeights w = query.biomeWeightsAt(p);
    TerrainMacroSample s = sampleMacro(noiseSet, p, settings);
    float h = computeHeight(s, noiseSet, p, settings, nullptr);
    BiomeWeights expected = sampleBiomeWeights(s, settings, h);
    CHECK(w.meadow == doctest::Approx(expected.meadow));
    CHECK(w.forest == doctest::Approx(expected.forest));
    CHECK(w.mountain == doctest::Approx(expected.mountain));
  }
}

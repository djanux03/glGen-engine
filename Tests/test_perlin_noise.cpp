#include <doctest/doctest.h>
#include "PerlinNoise.h"
#include <cmath>

TEST_CASE("PerlinNoise — deterministic with same seed") {
  PerlinNoise a(12345);
  PerlinNoise b(12345);
  for (int i = 0; i < 100; ++i) {
    float x = i * 0.37f, z = i * 0.53f;
    CHECK(a.noise(x, z) == doctest::Approx(b.noise(x, z)));
  }
}

TEST_CASE("PerlinNoise — different seeds differ") {
  PerlinNoise a(1), b(2);
  int diffs = 0;
  for (int i = 0; i < 100; ++i) {
    float x = i * 0.41f, z = i * 0.67f;
    if (std::abs(a.noise(x, z) - b.noise(x, z)) > 1e-6f) diffs++;
  }
  CHECK(diffs > 50);
}

TEST_CASE("PerlinNoise — output in [-1,1]") {
  PerlinNoise pn(42);
  for (int i = 0; i < 1000; ++i) {
    float x = (i * 0.123f) - 50.0f, z = (i * 0.456f) - 50.0f;
    float v = pn.noise(x, z);
    CHECK(v >= -1.0f);
    CHECK(v <= 1.0f);
  }
}

TEST_CASE("PerlinNoise — fbm normalized") {
  PerlinNoise pn(77);
  for (int i = 0; i < 500; ++i) {
    float x = (i * 0.31f) - 25.0f, z = (i * 0.47f) - 25.0f;
    float v = pn.fbm(x, z, 6);
    CHECK(v >= -1.5f);
    CHECK(v <= 1.5f);
  }
}

TEST_CASE("PerlinNoise — ridge noise non-negative") {
  PerlinNoise pn(99);
  for (int i = 0; i < 500; ++i) {
    float x = (i * 0.23f) - 30.0f, z = (i * 0.59f) - 30.0f;
    CHECK(pn.ridgeNoise(x, z, 4) >= -0.01f);
  }
}

TEST_CASE("PerlinNoise — spatial continuity") {
  PerlinNoise pn(42);
  float base = pn.noise(5.0f, 5.0f);
  float nearby = pn.noise(5.001f, 5.001f);
  CHECK(std::abs(base - nearby) < 0.1f);
}

TEST_CASE("PerlinNoise — integer coords produce zero") {
  PerlinNoise pn(42);
  CHECK(pn.noise(0.0f, 0.0f) == doctest::Approx(0.0f).epsilon(0.001f));
}

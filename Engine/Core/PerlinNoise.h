#pragma once
// PerlinNoise.h — Header-only Perlin noise with seed support
// Generates smooth, deterministic 2D noise for terrain heightmaps.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

class PerlinNoise {
public:
  explicit PerlinNoise(uint32_t seed = 0) {
    p.resize(256);
    std::iota(p.begin(), p.end(), 0);
    std::mt19937 rng(seed);
    std::shuffle(p.begin(), p.end(), rng);
    p.insert(p.end(), p.begin(), p.end()); // duplicate for overflow
  }

  // Single-octave 2D Perlin noise, returns value in roughly [-1, 1]
  float noise(float x, float z) const {
    int xi = (int)std::floor(x) & 255;
    int zi = (int)std::floor(z) & 255;

    float xf = x - std::floor(x);
    float zf = z - std::floor(z);

    float u = fade(xf);
    float v = fade(zf);

    int aa = p[p[xi] + zi];
    int ab = p[p[xi] + zi + 1];
    int ba = p[p[xi + 1] + zi];
    int bb = p[p[xi + 1] + zi + 1];

    float x1 = lerp(grad(aa, xf, zf), grad(ba, xf - 1, zf), u);
    float x2 = lerp(grad(ab, xf, zf - 1), grad(bb, xf - 1, zf - 1), u);

    return lerp(x1, x2, v);
  }

  // Fractal Brownian Motion — layers multiple octaves for natural terrain
  float fbm(float x, float z, int octaves = 6, float lacunarity = 2.0f,
            float gain = 0.5f) const {
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxAmplitude = 0.0f;

    for (int i = 0; i < octaves; ++i) {
      value += amplitude * noise(x * frequency, z * frequency);
      maxAmplitude += amplitude;
      amplitude *= gain;
      frequency *= lacunarity;
    }
    return value / maxAmplitude; // Normalize to [-1, 1]
  }

  // Ridge noise variant — creates sharp ridges like mountain ranges
  float ridgeNoise(float x, float z, int octaves = 6, float lacunarity = 2.0f,
                   float gain = 0.5f) const {
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float weight = 1.0f;

    for (int i = 0; i < octaves; ++i) {
      float n = noise(x * frequency, z * frequency);
      n = 1.0f - std::abs(n); // Create ridges
      n = n * n;              // Sharpen
      n *= weight;
      weight = std::clamp(n * 2.0f, 0.0f, 1.0f);
      value += amplitude * n;
      amplitude *= gain;
      frequency *= lacunarity;
    }
    return value;
  }

private:
  std::vector<int> p; // Permutation table

  static float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); // 6t^5 - 15t^4 + 10t^3
  }

  static float lerp(float a, float b, float t) { return a + t * (b - a); }

  static float grad(int hash, float x, float z) {
    int h = hash & 3;
    float u = (h < 2) ? x : z;
    float v = (h < 2) ? z : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
  }
};

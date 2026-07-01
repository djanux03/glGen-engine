#include "BlackHolePrecompute.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMu = 4.0f / 27.0f;

float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

float hash01(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return static_cast<float>(x & 0x00ffffffu) / 16777215.0f;
}

float smooth01(float edge0, float edge1, float x) {
  const float t = saturate((x - edge0) / std::max(edge1 - edge0, 0.0001f));
  return t * t * (3.0f - 2.0f * t);
}

glm::vec3 normalizeSafe(const glm::vec3 &v) {
  const float len2 = glm::dot(v, v);
  if (len2 <= 0.000001f)
    return glm::vec3(0.0f, 0.0f, 1.0f);
  return v / std::sqrt(len2);
}

glm::vec3 cubeDirection(int face, float u, float v) {
  const float x = u * 2.0f - 1.0f;
  const float y = v * 2.0f - 1.0f;
  switch (face) {
  case 0:
    return normalizeSafe(glm::vec3(1.0f, -y, -x));
  case 1:
    return normalizeSafe(glm::vec3(-1.0f, -y, x));
  case 2:
    return normalizeSafe(glm::vec3(x, 1.0f, y));
  case 3:
    return normalizeSafe(glm::vec3(x, -1.0f, -y));
  case 4:
    return normalizeSafe(glm::vec3(x, -y, 1.0f));
  default:
    return normalizeSafe(glm::vec3(-x, -y, -1.0f));
  }
}

glm::vec3 blackBodyApprox(float kelvin) {
  const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;

  if (t <= 66.0f) {
    r = 1.0f;
    g = saturate((99.4708025861f * std::log(t) - 161.1195681661f) / 255.0f);
    b = t <= 19.0f
            ? 0.0f
            : saturate((138.5177312231f * std::log(t - 10.0f) -
                        305.0447927307f) /
                       255.0f);
  } else {
    r = saturate((329.698727446f * std::pow(t - 60.0f, -0.1332047592f)) /
                 255.0f);
    g = saturate((288.1221695283f * std::pow(t - 60.0f, -0.0755148492f)) /
                 255.0f);
    b = 1.0f;
  }

  const float intensity = std::pow(std::clamp(kelvin / 8000.0f, 0.15f, 5.0f),
                                   0.32f);
  return glm::vec3(r, g, b) * intensity;
}

glm::vec3 directionToRgb(const glm::vec3 &dir, bool galaxy) {
  const glm::vec3 bandAxis = normalizeSafe(glm::vec3(-0.18f, 0.62f, 0.77f));
  const float band = std::exp(-std::pow(glm::dot(dir, bandAxis), 2.0f) *
                              (galaxy ? 9.0f : 18.0f));
  const float longitude = std::atan2(dir.z, dir.x);
  const float latitude = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
  const float swirls =
      0.5f + 0.5f * std::sin(longitude * 3.0f + latitude * 8.0f);
  const float dust =
      0.5f + 0.5f * std::sin(longitude * 17.0f - latitude * 11.0f + swirls);
  if (galaxy) {
    glm::vec3 cold(0.012f, 0.018f, 0.060f);
    glm::vec3 warm(0.160f, 0.135f, 0.090f);
    return cold + warm * band * (0.25f + 0.75f * dust);
  }

  glm::vec3 color(0.0f);
  const float u = longitude / (2.0f * kPi) + 0.5f;
  const float v = latitude / kPi + 0.5f;
  const float scale = 900.0f;
  const uint32_t ix = static_cast<uint32_t>(std::floor(u * scale));
  const uint32_t iy = static_cast<uint32_t>(std::floor(v * scale));
  const float h = hash01(ix * 73856093u ^ iy * 19349663u ^ 0x51ed270bu);
  if (h > 0.9965f) {
    const float temp = 2600.0f + hash01(ix * 31u ^ iy * 131u) * 12000.0f;
    color += blackBodyApprox(temp) * (0.45f + h * 3.2f);
  }
  color += glm::vec3(0.045f, 0.055f, 0.090f) * band * 0.20f;
  return color;
}

float eSquareFromDeflectionU(float u) {
  u = std::clamp(u, 0.0001f, 0.9999f);
  if (u < 0.5f) {
    const float d = (0.5f - u);
    return kMu * (1.0f - std::exp(-50.0f * d * d));
  }
  const float d = (u - 0.5f);
  const float denom = std::max(1.0f - std::exp(-50.0f * d * d), 0.0001f);
  return std::min(64.0f, kMu / denom);
}

float uApsis(float e2) {
  const float x = std::clamp((2.0f / kMu) * e2 - 1.0f, -1.0f, 1.0f);
  return 1.0f / 3.0f + (2.0f / 3.0f) * std::sin(std::asin(x) / 3.0f);
}

float inverseDeflectionV(float e2, float v) {
  v = saturate(v);
  if (e2 > kMu) {
    const float a = std::sqrt(2.0f / 3.0f);
    const float b = std::sqrt(1.0f / 3.0f);
    const float x = v * (a + b) - a;
    return x < 0.0f ? 2.0f / 3.0f - x * x : 2.0f / 3.0f + x * x;
  }
  const float ua = std::max(uApsis(e2), 0.0001f);
  const float oneMinus = 1.0f - v;
  return ua * (1.0f - oneMinus * oneMinus);
}

float phiUb(float e2) {
  return (1.0f + e2) /
         std::max(1.0f / 3.0f + 2.0f * e2 * std::sqrt(std::max(e2, 0.0f)),
                  0.0001f);
}
} // namespace

BlackHolePrecomputeData BlackHolePrecompute::generate() {
  const auto start = std::chrono::high_resolution_clock::now();
  BlackHolePrecomputeData data;

  data.rayDeflectionRG.resize(static_cast<size_t>(data.rayDeflectionWidth) *
                              data.rayDeflectionHeight * 2u);
  for (int y = 0; y < data.rayDeflectionHeight; ++y) {
    const float tv = (static_cast<float>(y) + 0.5f) /
                     static_cast<float>(data.rayDeflectionHeight);
    for (int x = 0; x < data.rayDeflectionWidth; ++x) {
      const float tu = (static_cast<float>(x) + 0.5f) /
                       static_cast<float>(data.rayDeflectionWidth);
      const float e2 = eSquareFromDeflectionU(tu);
      const float u = std::clamp(inverseDeflectionV(e2, tv), 0.0001f, 0.9999f);
      const float e = std::sqrt(std::max(e2, 0.000001f));
      const float critical = std::abs(e2 - kMu);
      const float whirl = -std::log(std::max(critical / kMu, 0.00002f));
      const float nearPhoton = std::exp(-std::pow((u - 2.0f / 3.0f) / 0.16f, 2.0f));
      float deflection = 0.36f / (e + 0.10f) + 0.18f / (u + 0.18f);
      deflection += nearPhoton * whirl * 0.22f;
      if (e2 > kMu && u > 0.66f)
        deflection += 0.85f * smooth01(0.66f, 1.0f, u);
      deflection = std::clamp(deflection, 0.0f, 18.0f);
      const float time = deflection * 0.55f + u * 0.35f;
      const size_t idx =
          (static_cast<size_t>(y) * data.rayDeflectionWidth + x) * 2u;
      data.rayDeflectionRG[idx + 0] = deflection;
      data.rayDeflectionRG[idx + 1] = time;
    }
  }

  data.rayInverseRadiusRG.resize(static_cast<size_t>(data.rayInverseRadiusWidth) *
                                 data.rayInverseRadiusHeight * 2u);
  for (int y = 0; y < data.rayInverseRadiusHeight; ++y) {
    const float tv = (static_cast<float>(y) + 0.5f) /
                     static_cast<float>(data.rayInverseRadiusHeight);
    for (int x = 0; x < data.rayInverseRadiusWidth; ++x) {
      const float tu = (static_cast<float>(x) + 0.5f) /
                       static_cast<float>(data.rayInverseRadiusWidth);
      const float e2 = std::max((1.0f / tu - 1.0f) / 6.0f, 0.00001f);
      const float phi = tv * phiUb(e2);
      const float e = std::sqrt(e2);
      const float bend = 1.0f + 0.32f / (std::abs(e2 - kMu) + 0.09f);
      float invR = e * std::sin(phi) /
                   std::max(1.0f + bend * e * (1.0f - std::cos(phi)) * 0.55f,
                            0.0001f);
      invR = std::clamp(invR, 0.0f, 1.0f);
      const float time = phi * (1.0f + 0.25f / (e + 0.08f));
      const size_t idx =
          (static_cast<size_t>(y) * data.rayInverseRadiusWidth + x) * 2u;
      data.rayInverseRadiusRG[idx + 0] = invR;
      data.rayInverseRadiusRG[idx + 1] = time;
    }
  }

  data.blackBodyRGB.resize(static_cast<size_t>(data.blackBodyWidth) * 3u);
  for (int x = 0; x < data.blackBodyWidth; ++x) {
    const float t = static_cast<float>(x) /
                    static_cast<float>(data.blackBodyWidth - 1);
    const float kelvin = 100.0f * std::exp(t * 6.0f);
    const glm::vec3 c = blackBodyApprox(kelvin);
    data.blackBodyRGB[x * 3 + 0] = c.r;
    data.blackBodyRGB[x * 3 + 1] = c.g;
    data.blackBodyRGB[x * 3 + 2] = c.b;
  }

  data.dopplerRGB.resize(static_cast<size_t>(data.dopplerSize) *
                         data.dopplerSize * data.dopplerSize * 3u);
  for (int z = 0; z < data.dopplerSize; ++z) {
    const float nz = static_cast<float>(z) / static_cast<float>(data.dopplerSize - 1);
    const float doppler = std::exp(std::tan((nz - 0.5f) * 3.0f) * 0.21f);
    for (int y = 0; y < data.dopplerSize; ++y) {
      const float ng = static_cast<float>(y) / static_cast<float>(data.dopplerSize - 1);
      for (int x = 0; x < data.dopplerSize; ++x) {
        const float nr = static_cast<float>(x) / static_cast<float>(data.dopplerSize - 1);
        glm::vec3 rgb(nr, ng * 0.5f, std::max(1.0f - nr - ng * 0.5f, 0.0f));
        const float beam = std::pow(std::clamp(doppler, 0.15f, 6.0f), 3.0f);
        rgb *= beam;
        rgb = glm::mix(rgb, glm::vec3(rgb.b, rgb.g, rgb.r) * beam,
                       saturate((doppler - 1.0f) * 0.35f));
        const size_t idx =
            ((static_cast<size_t>(z) * data.dopplerSize + y) *
                 data.dopplerSize +
             x) *
            3u;
        data.dopplerRGB[idx + 0] = rgb.r;
        data.dopplerRGB[idx + 1] = rgb.g;
        data.dopplerRGB[idx + 2] = rgb.b;
      }
    }
  }

  const size_t cubePixels =
      static_cast<size_t>(6) * data.skyCubeSize * data.skyCubeSize;
  data.starCubeRGB.resize(cubePixels * 3u);
  data.galaxyCubeRGB.resize(cubePixels * 3u);
  for (int face = 0; face < 6; ++face) {
    for (int y = 0; y < data.skyCubeSize; ++y) {
      for (int x = 0; x < data.skyCubeSize; ++x) {
        const glm::vec3 dir = cubeDirection(
            face, (static_cast<float>(x) + 0.5f) / data.skyCubeSize,
            (static_cast<float>(y) + 0.5f) / data.skyCubeSize);
        const size_t idx =
            ((static_cast<size_t>(face) * data.skyCubeSize + y) *
                 data.skyCubeSize +
             x) *
            3u;
        const glm::vec3 stars = directionToRgb(dir, false);
        const glm::vec3 galaxy = directionToRgb(dir, true);
        data.starCubeRGB[idx + 0] = stars.r;
        data.starCubeRGB[idx + 1] = stars.g;
        data.starCubeRGB[idx + 2] = stars.b;
        data.galaxyCubeRGB[idx + 0] = galaxy.r;
        data.galaxyCubeRGB[idx + 1] = galaxy.g;
        data.galaxyCubeRGB[idx + 2] = galaxy.b;
      }
    }
  }

  data.noiseR.resize(static_cast<size_t>(data.noiseSize) * data.noiseSize);
  for (int y = 0; y < data.noiseSize; ++y) {
    for (int x = 0; x < data.noiseSize; ++x) {
      const uint32_t seed = static_cast<uint32_t>(x) * 73856093u ^
                            static_cast<uint32_t>(y) * 19349663u ^
                            0x9e3779b9u;
      const float a = hash01(seed);
      const float b = hash01(seed ^ 0x68bc21ebu);
      const float c = hash01(seed ^ 0x02e5be93u);
      const float v = saturate(a * 0.58f + b * 0.28f + c * 0.14f);
      data.noiseR[static_cast<size_t>(y) * data.noiseSize + x] =
          static_cast<unsigned char>(std::round(v * 255.0f));
    }
  }

  const auto end = std::chrono::high_resolution_clock::now();
  data.generationMs =
      std::chrono::duration<double, std::milli>(end - start).count();
  return data;
}

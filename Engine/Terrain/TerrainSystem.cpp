#include "TerrainSystem.h"
#include "Assets/AssetManager.h"
#include "Assets/OBJModel.h"
#include "Assets/UFBXModel.h"
#include "ECS/Systems/PhysicsSystem.h"
#include "TerrainGpuRenderer.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <glm/gtx/matrix_decompose.hpp>

// ═══════════════════════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════════════════════

static constexpr float PI = 3.14159265359f;
static constexpr float TWO_PI = 6.28318530718f;
static constexpr int CYLINDER_SEGMENTS = 8;
static constexpr int CONE_SEGMENTS = 8;
static constexpr int SPHERE_RINGS = 4;
static constexpr int SPHERE_SECTORS = 6;
static constexpr int ROCK_SEGMENTS = 6;
static constexpr int WATER_RESOLUTION = 16;

static float saturate01(float v) { return std::clamp(v, 0.0f, 1.0f); }

static float smooth01(float a, float b, float v) {
  float t = saturate01((v - a) / std::max(0.0001f, b - a));
  return t * t * (3.0f - 2.0f * t);
}

struct TerrainMacroSample {
  float warpedX = 0.0f;
  float warpedZ = 0.0f;
  float temp = 0.5f;
  float moist = 0.5f;
  float continental = 0.5f;
  float mountainMask = 0.0f;
  float macroMountain = 0.0f;
  float macroValley = 0.0f;
  float macroRidge = 0.0f;
  float broadShape = 0.0f;
  float baseShape = 0.0f;
  float ridgeShape = 0.0f;
};

static TerrainMacroSample sampleTerrainMacro(const TerrainSettings &settings,
                                             const PerlinNoise &noise,
                                             const PerlinNoise &tempNoise,
                                             const PerlinNoise &moistNoise,
                                             float worldX, float worldZ) {
  TerrainMacroSample s;

  const float biomeFreq = std::max(0.0001f, settings.biomeScale);
  const float terrainFreq = std::max(0.0001f, settings.noiseFrequency);

  const float warpFreq = biomeFreq * 0.45f;
  const float warpAmp = settings.chunkWorldSize * 1.75f;
  const float warpX =
      tempNoise.noise(worldX * warpFreq + 31.7f, worldZ * warpFreq - 14.2f);
  const float warpZ =
      moistNoise.noise(worldX * warpFreq - 53.1f, worldZ * warpFreq + 22.8f);
  s.warpedX = worldX + warpX * warpAmp;
  s.warpedZ = worldZ + warpZ * warpAmp;

  const float climateFreq = biomeFreq * 0.55f;
  s.temp = tempNoise.fbm(s.warpedX * climateFreq + 100.0f,
                         s.warpedZ * climateFreq - 40.0f, 3, 2.0f, 0.5f);
  s.moist = moistNoise.fbm(s.warpedX * climateFreq - 140.0f,
                           s.warpedZ * climateFreq + 80.0f, 3, 2.0f, 0.5f);
  s.temp = s.temp * 0.5f + 0.5f;
  s.moist = s.moist * 0.5f + 0.5f;

  const float continentFreq = biomeFreq * 0.20f;
  float continentA =
      noise.fbm(s.warpedX * continentFreq - 500.0f,
                s.warpedZ * continentFreq + 270.0f, 4, 2.0f, 0.5f) *
          0.5f +
      0.5f;
  float continentB =
      tempNoise.fbm(s.warpedX * continentFreq * 0.6f + 810.0f,
                    s.warpedZ * continentFreq * 0.6f - 620.0f, 3, 2.0f, 0.5f) *
          0.5f +
      0.5f;
  s.continental = smooth01(0.18f, 0.82f, continentA * 0.7f + continentB * 0.3f);

  const float mountainFreq = biomeFreq * 0.35f;
  float mountainA =
      noise.ridgeNoise(s.warpedX * mountainFreq + 230.0f,
                       s.warpedZ * mountainFreq - 170.0f, 4, 2.05f, 0.5f);
  float mountainB =
      moistNoise.fbm(s.warpedX * mountainFreq * 0.55f - 900.0f,
                     s.warpedZ * mountainFreq * 0.55f + 300.0f, 3, 2.0f, 0.5f) *
          0.5f +
      0.5f;
  s.mountainMask =
      smooth01(0.50f, 0.88f, mountainA * 0.8f + mountainB * 0.2f) *
      smooth01(0.34f, 0.62f, s.continental);

  const float landscapeScale = std::max(0.35f, settings.landscapeScale);
  const float macroFreq = biomeFreq * (0.12f / landscapeScale);
  float macroA =
      noise.fbm(s.warpedX * macroFreq + 1240.0f,
                s.warpedZ * macroFreq - 860.0f, 3, 2.0f, 0.5f) *
          0.5f +
      0.5f;
  float macroB =
      tempNoise.fbm(s.warpedX * macroFreq * 0.55f - 410.0f,
                    s.warpedZ * macroFreq * 0.55f + 640.0f, 2, 2.0f, 0.5f) *
          0.5f +
      0.5f;
  float macroField = macroA * 0.72f + macroB * 0.28f;
  float mountainSpan = std::max(0.35f, settings.mountainSpan);
  float valleySpan = std::max(0.35f, settings.valleySpan);
  float mountainStart = 0.56f - (mountainSpan - 1.0f) * 0.10f;
  float mountainEnd = 0.88f - (mountainSpan - 1.0f) * 0.05f;
  float valleyStart = 0.16f;
  float valleyEnd = 0.44f + (valleySpan - 1.0f) * 0.18f;
  s.macroMountain = smooth01(mountainStart, mountainEnd, macroField);
  s.macroValley = 1.0f - smooth01(valleyStart, valleyEnd, macroField);
  s.macroRidge =
      noise.ridgeNoise(s.warpedX * macroFreq * 1.15f - 250.0f,
                       s.warpedZ * macroFreq * 1.15f + 510.0f, 3, 2.0f, 0.5f) *
      (0.45f + 0.55f * s.macroMountain);

  s.broadShape =
      noise.fbm(s.warpedX * terrainFreq * 0.18f - 320.0f,
                s.warpedZ * terrainFreq * 0.18f + 190.0f, 3, 2.0f, 0.5f);
  s.baseShape = noise.fbm(s.warpedX * terrainFreq * 0.65f,
                          s.warpedZ * terrainFreq * 0.65f, settings.octaves,
                          settings.lacunarity, settings.gain);
  s.ridgeShape = settings.useRidgeNoise
                     ? noise.ridgeNoise(s.warpedX * terrainFreq * 0.80f,
                                       s.warpedZ * terrainFreq * 0.80f,
                                       settings.octaves, settings.lacunarity,
                                       settings.gain)
                     : std::abs(noise.fbm(s.warpedX * terrainFreq * 0.80f,
                                          s.warpedZ * terrainFreq * 0.80f,
                                          settings.octaves, settings.lacunarity,
                                          settings.gain));
  return s;
}

static BiomeType classifyLandscapeBiome(const TerrainSettings &settings,
                                        const TerrainMacroSample &s) {
  if (settings.singleBiomeOnly)
    return BiomeType::Plains;

  if (s.continental < 0.33f)
    return BiomeType::Ocean;

  if (s.temp < 0.28f) {
    if (s.mountainMask > 0.52f)
      return BiomeType::Mountains;
    return BiomeType::Tundra;
  }

  if (s.mountainMask > 0.64f && s.continental > 0.42f)
    return BiomeType::Mountains;

  if (s.temp > 0.66f && s.moist < 0.38f && s.continental > 0.42f)
    return BiomeType::Desert;

  if (s.moist > 0.57f && s.continental > 0.38f)
    return BiomeType::Forest;

  return BiomeType::Plains;
}

static float computeTerrainHeightValue(const TerrainSettings &settings,
                                       const TerrainMacroSample &sample,
                                       const PerlinNoise &noise,
                                       const PerlinNoise &detailNoise) {
  const BiomeType biome = classifyLandscapeBiome(settings, sample);
  const float hs = settings.heightScale;
  const float land = smooth01(0.33f, 0.70f, sample.continental);
  const float inland = smooth01(0.48f, 0.85f, sample.continental);
  const float broad = sample.broadShape;
  const float rolling = broad * 0.65f + sample.baseShape * 0.35f;
  const float foothills = broad * 0.45f + sample.baseShape * 0.20f;
  const float mountains =
      foothills + sample.ridgeShape * (1.2f + sample.mountainMask * 1.4f);

  const float macroStrength = std::max(0.0f, settings.macroStrength);
  const float macroMountain =
      std::pow(saturate01(sample.macroMountain), 0.85f) *
      (0.55f + 0.45f * land);
  const float macroValley =
      std::pow(saturate01(sample.macroValley), 1.10f) *
      (0.70f + 0.30f * land);
  const float macroRidge = sample.macroRidge * macroMountain;
  const float macroMass = std::max(macroMountain, macroValley);

  float h = 0.0f;
  if (settings.singleBiomeOnly) {
    float macroShape = macroMountain * 1.45f + macroRidge * 0.55f -
                       macroValley * 0.95f;
    h = rolling * hs * 0.42f + broad * hs * 0.22f + inland * hs * 0.18f +
        macroShape * hs * macroStrength;
  } else {
    switch (biome) {
    case BiomeType::Ocean: {
      float shelf = smooth01(0.15f, 0.33f, sample.continental);
      float depth = (1.0f - shelf) * (hs * 2.4f + 6.0f);
      h = std::min(settings.seaLevel - depth + broad * hs * 0.12f -
                       macroValley * hs * 0.10f,
                   settings.seaLevel);
      break;
    }
    case BiomeType::Plains:
      h = rolling * hs * 0.40f + inland * hs * 0.20f +
          (macroMountain * 0.85f + macroRidge * 0.15f - macroValley * 0.78f) *
              hs * macroStrength;
      break;
    case BiomeType::Forest:
      h = (rolling * 0.72f + 0.06f) * hs * 0.58f + inland * hs * 0.16f +
          (macroMountain * 0.95f + macroRidge * 0.22f - macroValley * 0.82f) *
              hs * macroStrength;
      break;
    case BiomeType::Desert:
      h = (broad * 0.58f + std::abs(sample.baseShape) * 0.12f) * hs * 0.42f +
          land * hs * 0.10f +
          (macroMountain * 0.50f + macroRidge * 0.10f - macroValley * 0.60f) *
              hs * macroStrength;
      break;
    case BiomeType::Mountains:
      h = mountains * hs * 0.82f + inland * hs * 0.32f +
          (macroMountain * 1.95f + macroRidge * 0.85f - macroValley * 0.45f) *
              hs * macroStrength;
      break;
    case BiomeType::Tundra:
      h = (broad * 0.42f + sample.baseShape * 0.12f + 0.06f) * hs * 0.48f +
          land * hs * 0.10f +
          (macroMountain * 1.05f + macroRidge * 0.28f - macroValley * 0.70f) *
              hs * macroStrength;
      break;
    default:
      h = rolling * hs;
      break;
    }
  }

  float tDists[] = {std::abs(sample.temp - 0.28f),
                    std::abs(sample.temp - 0.66f)};
  float mDists[] = {std::abs(sample.moist - 0.38f),
                    std::abs(sample.moist - 0.57f),
                    std::abs(sample.continental - 0.33f),
                    std::abs(sample.mountainMask - 0.64f)};
  float minDist = 1.0f;
  for (float d : tDists)
    minDist = std::min(minDist, d);
  for (float d : mDists)
    minDist = std::min(minDist, d);

  const float blendZone = 0.10f;
  if (minDist < blendZone) {
    float blend = minDist / blendZone;
    float neutralH = rolling * hs * 0.38f + inland * hs * 0.10f +
                     (macroMountain * 0.75f - macroValley * 0.65f) * hs *
                         macroStrength;
    h = h * blend + neutralH * (1.0f - blend);
  }

  const float freq = std::max(0.0001f, settings.noiseFrequency);
  float erosion =
      noise.noise(sample.warpedX * freq * 2.0f, sample.warpedZ * freq * 2.0f) *
      0.10f;
  float micro = detailNoise.noise(sample.warpedX * freq * 4.0f + 150.0f,
                                  sample.warpedZ * freq * 4.0f - 70.0f) *
                0.035f;
  float fine = detailNoise.noise(sample.warpedX * freq * 8.0f - 320.0f,
                                 sample.warpedZ * freq * 8.0f + 260.0f) *
               0.012f;
  float detailStrength =
      (biome == BiomeType::Ocean) ? 0.18f : (0.42f + land * 0.18f);
  detailStrength *= 1.0f - macroMass * 0.35f;
  detailStrength = std::max(0.12f, detailStrength);
  h += (erosion + micro + fine) * hs * detailStrength;
  return h;
}

static void applyVegetationCullProfile(const TerrainSettings &settings,
                                       const std::string &prefabName,
                                       InstancedMeshComponent &inst) {
  const float meshDistance =
      std::max(40.0f, (float)settings.meshVegetationDistance);
  const float shadowDistance =
      std::max(0.0f, (float)settings.vegetationShadowDistance);

  inst.maxDrawDistance = meshDistance;
  inst.shadowMaxDrawDistance = std::min(meshDistance, shadowDistance);
  inst.instanceCullRadius = 6.0f;

  if (prefabName == "prefab_pine" || prefabName == "prefab_oak" ||
      prefabName == "prefab_birch" || prefabName == "prefab_deadtree") {
    inst.maxDrawDistance = meshDistance * 1.45f;
    inst.shadowMaxDrawDistance =
        std::min(inst.maxDrawDistance, shadowDistance);
    inst.instanceCullRadius = 9.0f;
  } else if (prefabName == "prefab_rock") {
    inst.maxDrawDistance = meshDistance * 0.82f;
    inst.shadowMaxDrawDistance =
        std::min(inst.maxDrawDistance, shadowDistance * 0.75f);
    inst.instanceCullRadius = 4.0f;
  } else if (prefabName == "prefab_grass" || prefabName == "prefab_flower") {
    inst.maxDrawDistance = meshDistance * 0.40f;
    inst.shadowMaxDrawDistance =
        std::min(inst.maxDrawDistance, shadowDistance * 0.35f);
    inst.instanceCullRadius = 1.4f;
  } else if (prefabName == "prefab_bush") {
    inst.maxDrawDistance = meshDistance * 0.64f;
    inst.shadowMaxDrawDistance =
        std::min(inst.maxDrawDistance, shadowDistance * 0.55f);
    inst.instanceCullRadius = 2.8f;
  } else if (prefabName == "prefab_cactus") {
    inst.maxDrawDistance = meshDistance * 1.10f;
    inst.shadowMaxDrawDistance =
        std::min(inst.maxDrawDistance, shadowDistance * 0.85f);
    inst.instanceCullRadius = 5.5f;
  }
}

static constexpr uint32_t INVALID_TREE_INSTANCE_INDEX =
    std::numeric_limits<uint32_t>::max();

static bool isInteractiveTreePrefab(const std::string &prefabName) {
  return prefabName == "prefab_pine" || prefabName == "prefab_oak" ||
         prefabName == "prefab_birch";
}

static TreeType chooseForestTreeType(const PerlinNoise &noise, float worldX,
                                     float worldZ) {
  const float species =
      noise.noise(worldX * 0.023f + 91.7f, worldZ * 0.023f - 34.2f) * 0.5f +
      0.5f;
  if (species < 0.58f)
    return TreeType::Pine;
  if (species < 0.84f)
    return TreeType::Oak;
  return TreeType::Birch;
}

static const char *treePrefabName(TreeType type) {
  switch (type) {
  case TreeType::Oak:
    return "prefab_oak";
  case TreeType::Birch:
    return "prefab_birch";
  default:
    return "prefab_pine";
  }
}

static bool shouldSpawnInteractiveTree(const TerrainSettings &settings,
                                       int cameraChunkX, int cameraChunkZ,
                                       int chunkX, int chunkZ,
                                       const glm::vec3 &worldPos,
                                       const PerlinNoise &noise,
                                       int currentChunkCount) {
  const int dx = std::abs(chunkX - cameraChunkX);
  const int dz = std::abs(chunkZ - cameraChunkZ);
  if (std::max(dx, dz) > std::max(0, settings.interactiveTreeChunkRadius))
    return false;
  if (currentChunkCount >= std::max(0, settings.maxInteractiveTreesPerChunk))
    return false;

  const float ratio = std::clamp(settings.interactiveTreeRatio, 0.0f, 1.0f);
  if (ratio >= 0.999f)
    return true;
  if (ratio <= 0.0f)
    return false;

  const float keep =
      noise.noise(worldPos.x * 0.071f + 17.3f, worldPos.z * 0.071f - 9.1f) *
          0.5f +
      0.5f;
  return keep <= ratio;
}

static bool chunkNeedsCollision(const TerrainSettings &settings,
                                int cameraChunkX, int cameraChunkZ,
                                int chunkX, int chunkZ) {
  const int radius = std::max(0, settings.collisionChunkRadius);
  const int dist =
      std::max(std::abs(chunkX - cameraChunkX), std::abs(chunkZ - cameraChunkZ));
  return dist <= radius;
}

// Generate a tapered cylinder (trunk shapes)
static void addCylinder(std::vector<OBJModel::VertexData> &verts,
                        glm::vec3 base, float radiusBot, float radiusTop,
                        float height, int segments, float biomeUV,
                        float materialUV = 0.0f) {
  for (int i = 0; i < segments; ++i) {
    float a0 = (float)i / segments * TWO_PI;
    float a1 = (float)(i + 1) / segments * TWO_PI;
    float c0 = std::cos(a0), s0 = std::sin(a0);
    float c1 = std::cos(a1), s1 = std::sin(a1);

    glm::vec3 bl(base.x + radiusBot * c0, base.y, base.z + radiusBot * s0);
    glm::vec3 br(base.x + radiusBot * c1, base.y, base.z + radiusBot * s1);
    glm::vec3 tl(base.x + radiusTop * c0, base.y + height,
                 base.z + radiusTop * s0);
    glm::vec3 tr(base.x + radiusTop * c1, base.y + height,
                 base.z + radiusTop * s1);

    float slope = (radiusBot - radiusTop) / height;
    glm::vec3 nbl = glm::normalize(glm::vec3(c0, slope, s0));
    glm::vec3 nbr = glm::normalize(glm::vec3(c1, slope, s1));
    glm::vec2 uv(materialUV, biomeUV);

    verts.push_back({bl, uv, nbl});
    verts.push_back({tl, uv, nbl});
    verts.push_back({br, uv, nbr});
    verts.push_back({br, uv, nbr});
    verts.push_back({tl, uv, nbl});
    verts.push_back({tr, uv, nbr});
  }
}

// Generate a cone (canopy shapes)
static void addCone(std::vector<OBJModel::VertexData> &verts, glm::vec3 base,
                    float radius, float height, int segments, float biomeUV,
                    float materialUV = 0.0f) {
  glm::vec3 tip = base + glm::vec3(0.0f, height, 0.0f);
  float slopeAngle = std::atan2(radius, height);
  float ny = std::sin(slopeAngle);
  float nr = std::cos(slopeAngle);

  for (int i = 0; i < segments; ++i) {
    float a0 = (float)i / segments * TWO_PI;
    float a1 = (float)(i + 1) / segments * TWO_PI;
    float c0 = std::cos(a0), s0 = std::sin(a0);
    float c1 = std::cos(a1), s1 = std::sin(a1);

    glm::vec3 p0(base.x + radius * c0, base.y, base.z + radius * s0);
    glm::vec3 p1(base.x + radius * c1, base.y, base.z + radius * s1);

    glm::vec3 n0 = glm::normalize(glm::vec3(nr * c0, ny, nr * s0));
    glm::vec3 n1 = glm::normalize(glm::vec3(nr * c1, ny, nr * s1));
    glm::vec3 nTip = glm::normalize(n0 + n1);
    glm::vec2 uv(materialUV, biomeUV);

    verts.push_back({p0, uv, n0});
    verts.push_back({tip, uv, nTip});
    verts.push_back({p1, uv, n1});
  }

  // Bottom cap
  glm::vec3 nDown(0, -1, 0);
  for (int i = 0; i < segments; ++i) {
    float a0 = (float)i / segments * TWO_PI;
    float a1 = (float)(i + 1) / segments * TWO_PI;
    glm::vec3 p0(base.x + radius * std::cos(a0), base.y,
                 base.z + radius * std::sin(a0));
    glm::vec3 p1(base.x + radius * std::cos(a1), base.y,
                 base.z + radius * std::sin(a1));
    glm::vec2 uv(materialUV, biomeUV);
    verts.push_back({base, uv, nDown});
    verts.push_back({p1, uv, nDown});
    verts.push_back({p0, uv, nDown});
  }
}

// Generate a rough sphere (for oak canopy, rocks)
static void addSphere(std::vector<OBJModel::VertexData> &verts,
                      glm::vec3 center, float radius, int rings, int sectors,
                      float biomeUV, float roughness = 0.0f,
                      const PerlinNoise *noiseGen = nullptr,
                      float materialUV = 0.0f) {
  for (int r = 0; r < rings; ++r) {
    float phi0 = PI * (float)r / rings;
    float phi1 = PI * (float)(r + 1) / rings;
    for (int s = 0; s < sectors; ++s) {
      float theta0 = TWO_PI * (float)s / sectors;
      float theta1 = TWO_PI * (float)(s + 1) / sectors;

      auto spherePoint = [&](float phi, float theta) -> glm::vec3 {
        float sp = std::sin(phi), cp = std::cos(phi);
        float st = std::sin(theta), ct = std::cos(theta);
        glm::vec3 dir(sp * ct, cp, sp * st);
        float r2 = radius;
        if (roughness > 0.0f && noiseGen) {
          r2 += roughness * noiseGen->noise(dir.x * 5.0f + center.x,
                                            dir.z * 5.0f + center.z);
        }
        return center + dir * r2;
      };

      glm::vec3 p00 = spherePoint(phi0, theta0);
      glm::vec3 p10 = spherePoint(phi0, theta1);
      glm::vec3 p01 = spherePoint(phi1, theta0);
      glm::vec3 p11 = spherePoint(phi1, theta1);

      glm::vec3 n00 = glm::normalize(p00 - center);
      glm::vec3 n10 = glm::normalize(p10 - center);
      glm::vec3 n01 = glm::normalize(p01 - center);
      glm::vec3 n11 = glm::normalize(p11 - center);
      glm::vec2 uv(materialUV, biomeUV);

      verts.push_back({p00, uv, n00});
      verts.push_back({p01, uv, n01});
      verts.push_back({p10, uv, n10});
      verts.push_back({p10, uv, n10});
      verts.push_back({p01, uv, n01});
      verts.push_back({p11, uv, n11});
    }
  }
}

// Generate a flat quad (water planes, flat decor)
static void addQuadPlane(std::vector<OBJModel::VertexData> &verts,
                         glm::vec3 center, float halfW, float halfZ,
                         float biomeUV, float materialUV = 0.0f) {
  glm::vec3 n(0, 1, 0);
  glm::vec2 uv(materialUV, biomeUV);
  glm::vec3 a(center.x - halfW, center.y, center.z - halfZ);
  glm::vec3 b(center.x + halfW, center.y, center.z - halfZ);
  glm::vec3 c(center.x + halfW, center.y, center.z + halfZ);
  glm::vec3 d(center.x - halfW, center.y, center.z + halfZ);

  verts.push_back({a, uv, n});
  verts.push_back({d, uv, n});
  verts.push_back({b, uv, n});
  verts.push_back({b, uv, n});
  verts.push_back({d, uv, n});
  verts.push_back({c, uv, n});
}

// Generate a box (for branches, cactus arms)
static void addBox(std::vector<OBJModel::VertexData> &verts, glm::vec3 minC,
                   glm::vec3 maxC, float biomeUV, float materialUV = 0.0f) {
  glm::vec3 corners[8] = {{minC.x, minC.y, minC.z}, {maxC.x, minC.y, minC.z},
                          {maxC.x, maxC.y, minC.z}, {minC.x, maxC.y, minC.z},
                          {minC.x, minC.y, maxC.z}, {maxC.x, minC.y, maxC.z},
                          {maxC.x, maxC.y, maxC.z}, {minC.x, maxC.y, maxC.z}};
  glm::vec2 uv(materialUV, biomeUV);
  auto face = [&](int a, int b, int c, int d, glm::vec3 n) {
    verts.push_back({corners[a], uv, n});
    verts.push_back({corners[b], uv, n});
    verts.push_back({corners[c], uv, n});
    verts.push_back({corners[a], uv, n});
    verts.push_back({corners[c], uv, n});
    verts.push_back({corners[d], uv, n});
  };
  face(0, 1, 2, 3, {0, 0, -1}); // front
  face(5, 4, 7, 6, {0, 0, 1});  // back
  face(4, 0, 3, 7, {-1, 0, 0}); // left
  face(1, 5, 6, 2, {1, 0, 0});  // right
  face(3, 2, 6, 7, {0, 1, 0});  // top
  face(4, 5, 1, 0, {0, -1, 0}); // bottom
}

static void addCylinderBetween(std::vector<OBJModel::VertexData> &verts,
                               glm::vec3 a, glm::vec3 b, float radiusA,
                               float radiusB, int segments, float biomeUV,
                               float materialUV) {
  glm::vec3 axis = b - a;
  const float height = glm::length(axis);
  if (height < 0.001f)
    return;
  axis /= height;
  glm::vec3 tangent =
      std::abs(axis.y) < 0.92f ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(1.0f, 0.0f, 0.0f);
  glm::vec3 right = glm::normalize(glm::cross(tangent, axis));
  glm::vec3 forward = glm::normalize(glm::cross(axis, right));
  glm::vec2 uv(materialUV, biomeUV);

  for (int i = 0; i < segments; ++i) {
    const float a0 = (float)i / segments * TWO_PI;
    const float a1 = (float)(i + 1) / segments * TWO_PI;
    const glm::vec3 r0 = right * std::cos(a0) + forward * std::sin(a0);
    const glm::vec3 r1 = right * std::cos(a1) + forward * std::sin(a1);
    const glm::vec3 p0 = a + r0 * radiusA;
    const glm::vec3 p1 = a + r1 * radiusA;
    const glm::vec3 q0 = b + r0 * radiusB;
    const glm::vec3 q1 = b + r1 * radiusB;
    const glm::vec3 n0 = glm::normalize(r0 + axis * ((radiusA - radiusB) / height));
    const glm::vec3 n1 = glm::normalize(r1 + axis * ((radiusA - radiusB) / height));

    verts.push_back({p0, uv, n0});
    verts.push_back({q0, uv, n0});
    verts.push_back({p1, uv, n1});
    verts.push_back({p1, uv, n1});
    verts.push_back({q0, uv, n0});
    verts.push_back({q1, uv, n1});
  }
}

static void addEllipsoid(std::vector<OBJModel::VertexData> &verts,
                         glm::vec3 center, glm::vec3 radii, int rings,
                         int sectors, float biomeUV, float materialUV,
                         float yaw = 0.0f) {
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  auto rotateY = [&](glm::vec3 p) {
    return glm::vec3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);
  };
  glm::vec2 uv(materialUV, biomeUV);

  for (int r = 0; r < rings; ++r) {
    const float phi0 = PI * (float)r / rings;
    const float phi1 = PI * (float)(r + 1) / rings;
    for (int s = 0; s < sectors; ++s) {
      const float theta0 = TWO_PI * (float)s / sectors;
      const float theta1 = TWO_PI * (float)(s + 1) / sectors;

      auto point = [&](float phi, float theta) {
        const float sp = std::sin(phi);
        glm::vec3 dir(sp * std::cos(theta), std::cos(phi),
                      sp * std::sin(theta));
        return center + rotateY(dir * radii);
      };
      auto normal = [&](glm::vec3 p) {
        glm::vec3 local = p - center;
        return glm::normalize(glm::vec3(local.x / std::max(0.001f, radii.x),
                                        local.y / std::max(0.001f, radii.y),
                                        local.z / std::max(0.001f, radii.z)));
      };

      const glm::vec3 p00 = point(phi0, theta0);
      const glm::vec3 p10 = point(phi0, theta1);
      const glm::vec3 p01 = point(phi1, theta0);
      const glm::vec3 p11 = point(phi1, theta1);
      verts.push_back({p00, uv, normal(p00)});
      verts.push_back({p01, uv, normal(p01)});
      verts.push_back({p10, uv, normal(p10)});
      verts.push_back({p10, uv, normal(p10)});
      verts.push_back({p01, uv, normal(p01)});
      verts.push_back({p11, uv, normal(p11)});
    }
  }
}

static void addPineTree(std::vector<OBJModel::VertexData> &verts) {
  constexpr float bark = 2.05f;
  constexpr float pineNeedles = 2.18f;
  addCylinder(verts, {0.0f, 0.0f, 0.0f}, 0.28f, 0.18f, 5.6f, 9, 0.4f, bark);
  addCylinderBetween(verts, {0.0f, 1.55f, 0.0f}, {1.05f, 2.10f, 0.20f}, 0.10f,
                     0.035f, 6, 0.4f, bark);
  addCylinderBetween(verts, {0.0f, 2.10f, 0.0f}, {-0.95f, 2.65f, -0.25f},
                     0.09f, 0.03f, 6, 0.4f, bark);
  addCylinderBetween(verts, {0.0f, 2.70f, 0.0f}, {0.72f, 3.25f, -0.55f},
                     0.075f, 0.025f, 6, 0.4f, bark);
  addCone(verts, {-0.08f, 1.15f, 0.05f}, 1.80f, 2.25f, 10, 0.4f,
          pineNeedles);
  addCone(verts, {0.14f, 2.10f, -0.10f}, 1.46f, 2.05f, 10, 0.4f,
          pineNeedles);
  addCone(verts, {-0.05f, 3.05f, 0.04f}, 1.08f, 1.75f, 10, 0.4f,
          pineNeedles);
  addCone(verts, {0.08f, 4.00f, -0.05f}, 0.68f, 1.40f, 9, 0.4f,
          pineNeedles);
}

static void addOakTree(std::vector<OBJModel::VertexData> &verts) {
  constexpr float bark = 2.05f;
  constexpr float broadLeaf = 2.32f;
  addCylinder(verts, {0.0f, 0.0f, 0.0f}, 0.36f, 0.25f, 3.45f, 9, 0.4f, bark);
  addCylinderBetween(verts, {0.0f, 2.15f, 0.0f}, {1.18f, 3.20f, 0.20f}, 0.15f,
                     0.06f, 7, 0.4f, bark);
  addCylinderBetween(verts, {0.0f, 2.35f, 0.0f}, {-1.10f, 3.12f, -0.35f},
                     0.14f, 0.055f, 7, 0.4f, bark);
  addCylinderBetween(verts, {0.0f, 2.70f, 0.0f}, {0.30f, 3.82f, -1.05f},
                     0.12f, 0.045f, 7, 0.4f, bark);
  addEllipsoid(verts, {0.00f, 4.00f, 0.00f}, {1.60f, 1.22f, 1.45f}, 5, 8,
               0.4f, broadLeaf, 0.30f);
  addEllipsoid(verts, {1.02f, 3.65f, 0.10f}, {1.08f, 0.86f, 0.98f}, 4, 8,
               0.4f, broadLeaf, -0.45f);
  addEllipsoid(verts, {-0.92f, 3.60f, -0.28f}, {1.00f, 0.82f, 0.94f}, 4, 8,
               0.4f, broadLeaf, 0.55f);
  addEllipsoid(verts, {0.22f, 4.42f, -0.78f}, {0.98f, 0.78f, 0.88f}, 4, 8,
               0.4f, broadLeaf, 1.10f);
}

static void addBirchTree(std::vector<OBJModel::VertexData> &verts) {
  constexpr float birchBark = 2.48f;
  constexpr float birchLeaf = 2.36f;
  addCylinder(verts, {0.0f, 0.0f, 0.0f}, 0.18f, 0.10f, 5.25f, 8, 0.4f,
              birchBark);
  addCylinderBetween(verts, {0.0f, 2.15f, 0.0f}, {0.76f, 3.25f, 0.18f}, 0.07f,
                     0.026f, 6, 0.4f, birchBark);
  addCylinderBetween(verts, {0.0f, 3.00f, 0.0f}, {-0.62f, 4.05f, -0.25f},
                     0.06f, 0.022f, 6, 0.4f, birchBark);
  addEllipsoid(verts, {0.00f, 4.25f, 0.0f}, {0.86f, 1.25f, 0.76f}, 5, 7,
               0.4f, birchLeaf, 0.15f);
  addEllipsoid(verts, {0.58f, 3.75f, 0.05f}, {0.62f, 0.92f, 0.52f}, 4, 7,
               0.4f, birchLeaf, -0.65f);
  addEllipsoid(verts, {-0.44f, 4.75f, -0.18f}, {0.54f, 0.82f, 0.48f}, 4, 7,
               0.4f, birchLeaf, 0.70f);
}

static void addDeadTree(std::vector<OBJModel::VertexData> &verts) {
  constexpr float deadwood = 2.62f;
  addCylinder(verts, {0.0f, 0.0f, 0.0f}, 0.24f, 0.13f, 3.9f, 7, 1.0f,
              deadwood);
  addCylinderBetween(verts, {0.0f, 1.65f, 0.0f}, {0.85f, 2.45f, -0.20f},
                     0.09f, 0.025f, 5, 1.0f, deadwood);
  addCylinderBetween(verts, {0.0f, 2.18f, 0.0f}, {-0.70f, 3.12f, 0.35f},
                     0.08f, 0.020f, 5, 1.0f, deadwood);
  addCylinderBetween(verts, {0.0f, 2.90f, 0.0f}, {0.24f, 3.85f, 0.18f},
                     0.06f, 0.0f, 5, 1.0f, deadwood);
}

// ═══════════════════════════════════════════════════════════════
// INITIALIZATION & LIFECYCLE
// ═══════════════════════════════════════════════════════════════

static void addGrassBlade(std::vector<OBJModel::VertexData> &verts,
                          glm::vec3 root, float width, float height,
                          float bend, float yaw, float materialUV) {
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  const glm::vec3 right(c, 0.0f, s);
  const glm::vec3 forward(-s, 0.0f, c);
  const glm::vec3 mid = root + glm::vec3(0.0f, height * 0.52f, 0.0f) +
                        forward * (bend * 0.36f);
  const glm::vec3 tip = root + glm::vec3(0.0f, height, 0.0f) +
                        forward * bend;
  const glm::vec3 baseL = root - right * (width * 0.5f);
  const glm::vec3 baseR = root + right * (width * 0.5f);
  const glm::vec3 midL = mid - right * (width * 0.24f);
  const glm::vec3 midR = mid + right * (width * 0.24f);
  const glm::vec3 normal = glm::normalize(glm::cross(midR - baseL, tip - baseL));
  const glm::vec2 uv(materialUV, 0.2f);

  verts.push_back({baseL, uv, normal});
  verts.push_back({midL, uv, normal});
  verts.push_back({baseR, uv, normal});
  verts.push_back({baseR, uv, normal});
  verts.push_back({midL, uv, normal});
  verts.push_back({midR, uv, normal});
  verts.push_back({midL, uv, normal});
  verts.push_back({tip, uv, normal});
  verts.push_back({midR, uv, normal});
}

static void addGrassCluster(std::vector<OBJModel::VertexData> &verts) {
  constexpr float grassBlade = 2.72f;
  constexpr int bladeCount = 12;
  for (int i = 0; i < bladeCount; ++i) {
    const float t = (float)i / (float)bladeCount;
    const float yaw = t * TWO_PI + std::sin(t * 17.0f) * 0.55f;
    const float ring = 0.05f + 0.24f * std::fmod(t * 3.17f, 1.0f);
    const glm::vec3 root(std::cos(yaw) * ring, 0.0f, std::sin(yaw) * ring);
    const float height = 0.62f + 0.44f * std::fmod(t * 5.71f + 0.18f, 1.0f);
    const float width = 0.055f + 0.028f * std::fmod(t * 7.13f + 0.33f, 1.0f);
    const float bend = 0.10f + 0.18f * std::fmod(t * 4.41f + 0.51f, 1.0f);
    addGrassBlade(verts, root, width, height, bend, yaw, grassBlade);
  }

  addGrassBlade(verts, {0.06f, 0.0f, -0.05f}, 0.075f, 1.18f, 0.18f, 0.45f,
                grassBlade);
  addGrassBlade(verts, {-0.08f, 0.0f, 0.04f}, 0.065f, 1.02f, 0.22f, 2.65f,
                grassBlade);
}

static void addFlower(std::vector<OBJModel::VertexData> &verts) {
  constexpr float grassStem = 2.76f;
  constexpr float flowerPetal = 2.84f;
  addCylinder(verts, {0.0f, 0.0f, 0.0f}, 0.018f, 0.012f, 0.72f, 5, 0.2f,
              grassStem);

  const glm::vec3 center(0.0f, 0.76f, 0.0f);
  for (int i = 0; i < 6; ++i) {
    const float yaw = (float)i / 6.0f * TWO_PI;
    const glm::vec3 dir(std::cos(yaw), 0.18f, std::sin(yaw));
    const glm::vec3 side(-std::sin(yaw), 0.0f, std::cos(yaw));
    const glm::vec3 tip = center + glm::normalize(dir) * 0.18f;
    const glm::vec3 left = center + side * 0.045f;
    const glm::vec3 right = center - side * 0.045f;
    const glm::vec3 normal = glm::normalize(glm::cross(tip - left, right - left));
    const glm::vec2 uv(flowerPetal, 0.2f);
    verts.push_back({center, uv, normal});
    verts.push_back({left, uv, normal});
    verts.push_back({tip, uv, normal});
    verts.push_back({center, uv, normal});
    verts.push_back({tip, uv, normal});
    verts.push_back({right, uv, normal});
  }
}

TerrainSystem::~TerrainSystem() { shutdown(); }

void TerrainSystem::init(const TerrainSettings &settings, Scene &scene,
                         AssetManager *assets) {
  mSettings = settings;
  mScene = &scene;
  mAssets = assets;
  mNoise = PerlinNoise(mSettings.seed);
  mTempNoise = PerlinNoise(mSettings.seed + 1000);
  mMoistNoise = PerlinNoise(mSettings.seed + 2000);
  mTreeNoise = PerlinNoise(mSettings.seed + 3000);
  mRockNoise = PerlinNoise(mSettings.seed + 4000);
  mDetailNoise = PerlinNoise(mSettings.seed + 5000);
  mLastCameraChunk = {INT_MAX, INT_MAX};
  ++mGenerationId;
  mStats = {};
  mGpuMainPassStats = {};
  mGpuShadowPassStats = {};
  mHasGpuMainPassStats = false;
  mHasGpuShadowPassStats = false;
  mGpuTerrainFallbackActive = false;

  ensureGpuRenderer();
  initPrefabs();

  std::cout << "[Terrain] Initialized with seed " << mSettings.seed
            << " | biomes, vegetation, rocks, water" << std::endl;
}

void TerrainSystem::applySettings(const TerrainSettings &settings) {
  const bool gpuShapeChanged =
      settings.chunkSize != mSettings.chunkSize ||
      settings.terrainGpuPageCapacity != mSettings.terrainGpuPageCapacity ||
      settings.useGpuTerrain != mSettings.useGpuTerrain;
  if (settings.chunkSize != mSettings.chunkSize ||
      std::abs(settings.chunkWorldSize - mSettings.chunkWorldSize) > 0.001f) {
    mHeightOffsets.clear();
    mPaintedInstances.clear();
  }
  mSettings = settings;
  if (gpuShapeChanged && mGpuRenderer) {
    mGpuRenderer->shutdown();
    mGpuRenderer.reset();
  }
  ensureGpuRenderer();
}

void TerrainSystem::regenerate() {
  // Wait for any in-flight async chunk jobs to finish before clearing state
  for (auto &f : mChunkFutures)
    if (f.valid())
      f.wait();
  mChunkFutures.clear();
  mInFlight.clear();

  // Discard any pending results that are no longer needed
  {
    std::lock_guard<std::mutex> lk(mPendingMutex);
    mPendingReady.clear();
  }
  mPendingCollisionUpdates.clear();
  ++mGenerationId;
  if (mGpuRenderer)
    mGpuRenderer->clear();

  std::vector<ChunkCoord> all;
  for (auto &[coord, data] : mChunks)
    all.push_back(coord);
  for (auto &coord : all)
    unloadChunk(coord.x, coord.z);

  clearPrefabs();
  initPrefabs();

  mNoise = PerlinNoise(mSettings.seed);
  mTempNoise = PerlinNoise(mSettings.seed + 1000);
  mMoistNoise = PerlinNoise(mSettings.seed + 2000);
  mTreeNoise = PerlinNoise(mSettings.seed + 3000);
  mRockNoise = PerlinNoise(mSettings.seed + 4000);
  mDetailNoise = PerlinNoise(mSettings.seed + 5000);
  mLastCameraChunk = {INT_MAX, INT_MAX};
  mStats = {};
  mGpuMainPassStats = {};
  mGpuShadowPassStats = {};
  mHasGpuMainPassStats = false;
  mHasGpuShadowPassStats = false;
  mGpuTerrainFallbackActive = false;
  ensureGpuRenderer();
  std::cout << "[Terrain] Regenerated with seed " << mSettings.seed
            << std::endl;
}

void TerrainSystem::shutdown() {
  for (auto &f : mChunkFutures)
    if (f.valid())
      f.wait();
  mChunkFutures.clear();
  mInFlight.clear();
  {
    std::lock_guard<std::mutex> lk(mPendingMutex);
    mPendingReady.clear();
  }
  mPendingCollisionUpdates.clear();
  ++mGenerationId;
  std::vector<ChunkCoord> all;
  for (auto &[coord, data] : mChunks)
    all.push_back(coord);
  for (auto &coord : all)
    unloadChunk(coord.x, coord.z);
  clearPrefabs();
  if (mGpuRenderer) {
    mGpuRenderer->shutdown();
    mGpuRenderer.reset();
  }
  mStats = {};
  mGpuMainPassStats = {};
  mGpuShadowPassStats = {};
  mHasGpuMainPassStats = false;
  mHasGpuShadowPassStats = false;
  mGpuTerrainFallbackActive = false;
}

bool TerrainSystem::ensureGpuRenderer() {
  mStats.gpuTerrainActive = false;
  if (!mSettings.enabled || !mSettings.useGpuTerrain)
    return false;
  if (!mGpuRenderer)
    mGpuRenderer = std::make_unique<TerrainGpuRenderer>();
  mGpuRenderer->setSubmissionBackend(mSubmissionBackend);
  if (!mGpuRenderer->active()) {
    if (!mGpuRenderer->initialize(mSettings.chunkSize,
                                  mSettings.terrainGpuPageCapacity)) {
      mGpuTerrainFallbackActive = true;
      mStats.gpuTerrainFallback = true;
      return false;
    }
  }
  mStats.gpuTerrainActive = true;
  mStats.gpuTerrainFallback = mGpuTerrainFallbackActive;
  return true;
}

bool TerrainSystem::gpuTerrainActive() const {
  return mSettings.enabled && mSettings.useGpuTerrain && mGpuRenderer &&
         mGpuRenderer->active();
}

void TerrainSystem::applyGpuRenderStats(const TerrainGpuRendererStats &gs) {
  mStats.visibleChunks = gs.visibleChunks;
  mStats.horizonCulledChunks = gs.horizonCulledChunks;
  mStats.frustumCulledChunks = gs.frustumCulledChunks;
  mStats.terrainDrawCalls = gs.drawCalls;
  mStats.terrainGpuPagesUsed = gs.pagesUsed;
  mStats.terrainGpuInstanceUploads = gs.instanceUploads;
  mStats.terrainGpuInstanceUploadSkips = gs.instanceUploadSkips;
  mStats.terrainGpuInstanceUploadBytes = gs.instanceUploadBytes;
  mStats.gpuTerrainActive = gs.active;
  mStats.gpuTerrainFallback = gs.fallback || mGpuTerrainFallbackActive;
}

void TerrainSystem::syncStreamingStats() {
  if (!mGpuRenderer) {
    mStats.visibleChunks = mStats.loadedChunks;
    mStats.horizonCulledChunks = 0;
    mStats.frustumCulledChunks = 0;
    mStats.terrainDrawCalls = 0;
    mStats.terrainGpuPagesUsed = 0;
    mStats.terrainGpuInstanceUploads = 0;
    mStats.terrainGpuInstanceUploadSkips = 0;
    mStats.terrainGpuInstanceUploadBytes = 0;
    mStats.gpuTerrainActive = false;
    mStats.gpuTerrainFallback = mGpuTerrainFallbackActive;
  } else {
    mStats.gpuTerrainActive = mGpuRenderer->active();
    mStats.gpuTerrainFallback = mGpuTerrainFallbackActive;
  }

  mStats.pendingJobs = (int)mInFlight.size();
  {
    std::lock_guard<std::mutex> lk(mPendingMutex);
    mStats.pendingUploads = (int)mPendingReady.size();
  }
  int collisionBodies = 0;
  for (const auto &[_, cd] : mChunks) {
    if (cd.physicsBodyId != 0xFFFFFFFF)
      ++collisionBodies;
  }
  mStats.collisionBodies = collisionBodies;
}

void TerrainSystem::syncGpuStats() {
  if (mGpuRenderer && mHasGpuMainPassStats) {
    applyGpuRenderStats(mGpuMainPassStats);
  } else if (!mGpuRenderer) {
    mStats.visibleChunks = mStats.loadedChunks;
    mStats.horizonCulledChunks = 0;
    mStats.frustumCulledChunks = 0;
    mStats.terrainDrawCalls = 0;
    mStats.terrainGpuPagesUsed = 0;
    mStats.terrainGpuInstanceUploads = 0;
    mStats.terrainGpuInstanceUploadSkips = 0;
    mStats.terrainGpuInstanceUploadBytes = 0;
    mStats.gpuTerrainActive = false;
    mStats.gpuTerrainFallback = mGpuTerrainFallbackActive;
  }
  syncStreamingStats();
}

void TerrainSystem::renderGpuTerrain(Shader &shader,
                                     const glm::mat4 &viewProjection,
                                     const glm::vec3 &cameraPos,
                                     bool shadowPass) {
  if (!ensureGpuRenderer())
    return;
  const TerrainGpuRendererStats passStats =
      mGpuRenderer->render(shader, viewProjection, cameraPos, shadowPass,
                           mSettings.enableHorizonCulling,
                           mSettings.horizonCullingSectors);
  if (shadowPass) {
    mGpuShadowPassStats = passStats;
    mHasGpuShadowPassStats = true;
  } else {
    mGpuMainPassStats = passStats;
    mHasGpuMainPassStats = true;
    applyGpuRenderStats(passStats);
  }
  syncStreamingStats();
}

// ═══════════════════════════════════════════════════════════════
// BIOME DETERMINATION
// ═══════════════════════════════════════════════════════════════

BiomeType TerrainSystem::getBiome(float worldX, float worldZ) const {
  TerrainMacroSample sample =
      sampleTerrainMacro(mSettings, mNoise, mTempNoise, mMoistNoise, worldX,
                         worldZ);
  return classifyLandscapeBiome(mSettings, sample);
}

BiomeType TerrainSystem::getChunkDominantBiome(int cx, int cz) const {
  float ws = mSettings.chunkWorldSize;
  float ox = cx * ws + ws * 0.5f;
  float oz = cz * ws + ws * 0.5f;
  // Sample center and 4 corners, take majority
  int counts[6] = {};
  counts[(int)getBiome(ox, oz)]++;
  counts[(int)getBiome(ox - ws * 0.3f, oz - ws * 0.3f)]++;
  counts[(int)getBiome(ox + ws * 0.3f, oz - ws * 0.3f)]++;
  counts[(int)getBiome(ox - ws * 0.3f, oz + ws * 0.3f)]++;
  counts[(int)getBiome(ox + ws * 0.3f, oz + ws * 0.3f)]++;

  int maxIdx = 0;
  for (int i = 1; i < 6; ++i)
    if (counts[i] > counts[maxIdx])
      maxIdx = i;
  return (BiomeType)maxIdx;
}

// ═══════════════════════════════════════════════════════════════
// HEIGHT COMPUTATION
// ═══════════════════════════════════════════════════════════════

float TerrainSystem::sampleBaseNoise(float worldX, float worldZ) const {
  TerrainMacroSample sample =
      sampleTerrainMacro(mSettings, mNoise, mTempNoise, mMoistNoise, worldX,
                         worldZ);
  return sample.baseShape;
}

float TerrainSystem::sampleRidgeNoise(float worldX, float worldZ) const {
  TerrainMacroSample sample =
      sampleTerrainMacro(mSettings, mNoise, mTempNoise, mMoistNoise, worldX,
                         worldZ);
  return sample.ridgeShape;
}

float TerrainSystem::shapeBiomeHeight(BiomeType biome, float baseH,
                                      float ridgeH) const {
  float hs = mSettings.heightScale;
  switch (biome) {
  case BiomeType::Ocean:
    return std::min(mSettings.seaLevel - hs * 0.6f + baseH * hs * 0.10f,
                    mSettings.seaLevel);
  case BiomeType::Plains:
    return baseH * hs * 0.45f;
  case BiomeType::Forest:
    return (baseH * 0.55f + 0.08f) * hs;
  case BiomeType::Desert:
    return (baseH * 0.35f + std::abs(baseH) * 0.18f) * hs;
  case BiomeType::Mountains:
    return (baseH * 0.20f + ridgeH * 1.8f) * hs;
  case BiomeType::Tundra:
    return (baseH * 0.22f + 0.12f) * hs;
  default:
    return baseH * hs;
  }
}

float TerrainSystem::sampleHeight(float worldX, float worldZ) const {
  TerrainMacroSample sample =
      sampleTerrainMacro(mSettings, mNoise, mTempNoise, mMoistNoise, worldX,
                         worldZ);
  float h = computeTerrainHeightValue(mSettings, sample, mNoise, mDetailNoise);

  // Apply sculpted height offsets if any exist for this chunk.
  if (!mHeightOffsets.empty()) {
    const float ws = mSettings.chunkWorldSize;
    if (ws > 0.0f) {
      const int cx = (int)std::floor(worldX / ws);
      const int cz = (int)std::floor(worldZ / ws);
      auto it = mHeightOffsets.find({cx, cz});
      if (it != mHeightOffsets.end()) {
        const auto &data = it->second;
        const uint32_t sc = data.sampleCount;
        if (sc >= 2 && data.offsets.size() == (size_t)sc * sc) {
          const float originX = cx * ws;
          const float originZ = cz * ws;
          const float gridSize = (float)(sc - 1);
          const float fx = (worldX - originX) / ws * gridSize;
          const float fz = (worldZ - originZ) / ws * gridSize;
          const int ix = std::clamp((int)std::floor(fx), 0, (int)sc - 1);
          const int iz = std::clamp((int)std::floor(fz), 0, (int)sc - 1);
          const int ix1 = std::min(ix + 1, (int)sc - 1);
          const int iz1 = std::min(iz + 1, (int)sc - 1);
          const float tx = fx - (float)ix;
          const float tz = fz - (float)iz;
          const size_t i00 = (size_t)iz * sc + (size_t)ix;
          const size_t i10 = (size_t)iz * sc + (size_t)ix1;
          const size_t i01 = (size_t)iz1 * sc + (size_t)ix;
          const size_t i11 = (size_t)iz1 * sc + (size_t)ix1;
          const float h00 = data.offsets[i00];
          const float h10 = data.offsets[i10];
          const float h01 = data.offsets[i01];
          const float h11 = data.offsets[i11];
          const float h0 = h00 + (h10 - h00) * tx;
          const float h1 = h01 + (h11 - h01) * tx;
          h += h0 + (h1 - h0) * tz;
        }
      }
    }
  }

  return h;
}

// ═══════════════════════════════════════════════════════════════
// TERRAIN QUERIES API
// ═══════════════════════════════════════════════════════════════

float TerrainSystem::getHeightAt(float worldX, float worldZ) const {
  return sampleHeight(worldX, worldZ);
}

BiomeType TerrainSystem::getBiomeAt(float worldX, float worldZ) const {
  return getBiome(worldX, worldZ);
}

glm::vec3 TerrainSystem::getNormalAt(float worldX, float worldZ) const {
  float eps = 0.5f;
  float hL = sampleHeight(worldX - eps, worldZ);
  float hR = sampleHeight(worldX + eps, worldZ);
  float hD = sampleHeight(worldX, worldZ - eps);
  float hU = sampleHeight(worldX, worldZ + eps);
  return glm::normalize(glm::vec3(-(hR - hL), 2.0f * eps, -(hU - hD)));
}

float TerrainSystem::getSlopeAt(float worldX, float worldZ) const {
  glm::vec3 n = getNormalAt(worldX, worldZ);
  return 1.0f - std::abs(glm::dot(n, glm::vec3(0, 1, 0)));
}

bool TerrainSystem::isUnderwater(float worldX, float worldZ) const {
  return sampleHeight(worldX, worldZ) < mSettings.seaLevel;
}

bool TerrainSystem::isChunkLoadedAt(float worldX, float worldZ) const {
  if (mChunks.empty() || mSettings.chunkWorldSize <= 0.0f)
    return false;

  const int cx = (int)std::floor(worldX / mSettings.chunkWorldSize);
  const int cz = (int)std::floor(worldZ / mSettings.chunkWorldSize);
  return mChunks.find({cx, cz}) != mChunks.end();
}

glm::vec2 TerrainSystem::clampXZToLoadedRegion(float worldX, float worldZ,
                                               float margin) const {
  if (mChunks.empty() || mSettings.chunkWorldSize <= 0.0f)
    return {worldX, worldZ};

  const float ws = mSettings.chunkWorldSize;
  const float safeMargin = std::max(0.0f, margin);
  float bestX = worldX;
  float bestZ = worldZ;
  float bestDist2 = std::numeric_limits<float>::infinity();

  for (const auto &[coord, _] : mChunks) {
    const float chunkMinX = coord.x * ws;
    const float chunkMinZ = coord.z * ws;
    float minX = chunkMinX + safeMargin;
    float maxX = chunkMinX + ws - safeMargin;
    float minZ = chunkMinZ + safeMargin;
    float maxZ = chunkMinZ + ws - safeMargin;

    // Large margins can invert bounds for tiny chunks; collapse to chunk
    // center.
    if (minX > maxX) {
      const float cx = chunkMinX + ws * 0.5f;
      minX = cx;
      maxX = cx;
    }
    if (minZ > maxZ) {
      const float cz = chunkMinZ + ws * 0.5f;
      minZ = cz;
      maxZ = cz;
    }

    const float clampedX = std::clamp(worldX, minX, maxX);
    const float clampedZ = std::clamp(worldZ, minZ, maxZ);
    const float dx = clampedX - worldX;
    const float dz = clampedZ - worldZ;
    const float dist2 = dx * dx + dz * dz;

    if (dist2 < bestDist2) {
      bestDist2 = dist2;
      bestX = clampedX;
      bestZ = clampedZ;
      if (dist2 == 0.0f)
        break;
    }
  }

  return {bestX, bestZ};
}

bool TerrainSystem::applyHeightBrush(const glm::vec3 &center, float radius,
                                     float delta) {
  if (!mSettings.enabled || radius <= 0.0f || delta == 0.0f)
    return false;

  const float ws = mSettings.chunkWorldSize;
  if (ws <= 0.0f)
    return false;
  const int size = mSettings.chunkSize;
  const float step = ws / (float)size;
  const uint32_t sc = (uint32_t)(size + 1);

  const int minCx = (int)std::floor((center.x - radius) / ws);
  const int maxCx = (int)std::floor((center.x + radius) / ws);
  const int minCz = (int)std::floor((center.z - radius) / ws);
  const int maxCz = (int)std::floor((center.z + radius) / ws);

  bool changed = false;
  for (int cz = minCz; cz <= maxCz; ++cz) {
    for (int cx = minCx; cx <= maxCx; ++cx) {
      HeightOffsetData &data = mHeightOffsets[{cx, cz}];
      if (data.sampleCount != sc) {
        data.sampleCount = sc;
        data.offsets.assign((size_t)sc * sc, 0.0f);
      }
      const float ox = cx * ws;
      const float oz = cz * ws;
      const int minX = std::clamp(
          (int)std::floor((center.x - radius - ox) / step), 0, size);
      const int maxX = std::clamp(
          (int)std::floor((center.x + radius - ox) / step), 0, size);
      const int minZ = std::clamp(
          (int)std::floor((center.z - radius - oz) / step), 0, size);
      const int maxZ = std::clamp(
          (int)std::floor((center.z + radius - oz) / step), 0, size);

      for (int z = minZ; z <= maxZ; ++z) {
        for (int x = minX; x <= maxX; ++x) {
          float wx = ox + x * step;
          float wz = oz + z * step;
          float dx = wx - center.x;
          float dz = wz - center.z;
          float dist = std::sqrt(dx * dx + dz * dz);
          if (dist > radius)
            continue;
          float falloff = 1.0f - (dist / radius);
          data.offsets[(size_t)z * sc + x] += delta * falloff;
          changed = true;
        }
      }
      if (changed && mChunks.find({cx, cz}) != mChunks.end())
        rebuildChunkTerrain(cx, cz);
    }
  }

  return changed;
}

bool TerrainSystem::applyVegetationBrush(const glm::vec3 &center, float radius,
                                         const std::string &prefabName,
                                         bool add, int count) {
  if (!mSettings.enabled || radius <= 0.0f || prefabName.empty())
    return false;
  if (!mScene)
    return false;
  auto itPrefab = mPrefabs.find(prefabName);
  if (itPrefab == mPrefabs.end())
    return false;

  const float ws = mSettings.chunkWorldSize;
  if (ws <= 0.0f)
    return false;

  const int minCx = (int)std::floor((center.x - radius) / ws);
  const int maxCx = (int)std::floor((center.x + radius) / ws);
  const int minCz = (int)std::floor((center.z - radius) / ws);
  const int maxCz = (int)std::floor((center.z + radius) / ws);

  bool changed = false;
  auto &reg = mScene->registry();

  if (!add) {
    for (int cz = minCz; cz <= maxCz; ++cz) {
      for (int cx = minCx; cx <= maxCx; ++cx) {
        auto itChunk = mChunks.find({cx, cz});
        if (itChunk == mChunks.end())
          continue;
        ChunkData &cd = itChunk->second;
        auto itM = cd.prefabInstanceMatrices.find(prefabName);
        if (itM == cd.prefabInstanceMatrices.end())
          continue;
        auto &mats = itM->second;
        auto &ents = cd.prefabInstanceEntities[prefabName];
        auto &globals = cd.prefabInstanceGlobalIndices[prefabName];

        std::vector<glm::mat4> newMats;
        std::vector<uint32_t> newEnts;
        std::vector<uint32_t> newGlobals;
        newMats.reserve(mats.size());
        newEnts.reserve(ents.size());
        newGlobals.reserve(globals.size());

        for (size_t i = 0; i < mats.size(); ++i) {
          glm::vec3 pos = glm::vec3(mats[i][3]);
          float dx = pos.x - center.x;
          float dz = pos.z - center.z;
          float dist = std::sqrt(dx * dx + dz * dz);
          bool remove = dist <= radius;
          const uint32_t eid = (i < ents.size()) ? ents[i] : 0;
          if (remove) {
            if (eid != 0) {
              if (mPhysicsSystem && reg.has<RigidbodyComponent>(eid)) {
                auto &rb = reg.get<RigidbodyComponent>(eid);
                if (rb.bodyID != 0xFFFFFFFF)
                  mPhysicsSystem->removeBody(rb.bodyID);
              }
              mScene->deleteEntity(eid);
            }
            if (prefabName == "prefab_pine") {
              if (eid != 0 && cd.treeCount > 0)
                cd.treeCount--;
              if (eid != 0 && mStats.totalTreeEntities > 0)
                mStats.totalTreeEntities--;
            } else if (prefabName == "prefab_rock") {
              if (cd.rockCount > 0)
                cd.rockCount--;
              if (mStats.totalRockEntities > 0)
                mStats.totalRockEntities--;
            }
            changed = true;
          } else {
            newMats.push_back(mats[i]);
            newEnts.push_back(eid);
            if (i < globals.size())
              newGlobals.push_back(globals[i]);
            else
              newGlobals.push_back(INVALID_TREE_INSTANCE_INDEX);
          }
        }

        mats.swap(newMats);
        ents.swap(newEnts);
        globals.swap(newGlobals);
        cd.prefabInstanceCounts[prefabName] = (int)mats.size();
        // Update painted cache for this chunk
        auto itPaint = mPaintedInstances.find({cx, cz});
        if (itPaint != mPaintedInstances.end()) {
          auto itPM = itPaint->second.prefabMatrices.find(prefabName);
          if (itPM != itPaint->second.prefabMatrices.end()) {
            std::vector<glm::mat4> newPainted;
            newPainted.reserve(itPM->second.size());
            for (const auto &pm : itPM->second) {
              glm::vec3 pos = glm::vec3(pm[3]);
              float dx = pos.x - center.x;
              float dz = pos.z - center.z;
              float dist = std::sqrt(dx * dx + dz * dz);
              if (dist > radius)
                newPainted.push_back(pm);
              else
                changed = true;
            }
            itPM->second.swap(newPainted);
            if (itPM->second.empty())
              itPaint->second.prefabMatrices.erase(itPM);
            if (itPaint->second.prefabMatrices.empty())
              mPaintedInstances.erase(itPaint);
          }
        }
      }
    }
  } else {
    const int spawnCount = std::max(1, count);
    for (int i = 0; i < spawnCount; ++i) {
      float a = ((float)std::rand() / (float)RAND_MAX) * TWO_PI;
      float r = std::sqrt((float)std::rand() / (float)RAND_MAX) * radius;
      float wx = center.x + std::cos(a) * r;
      float wz = center.z + std::sin(a) * r;
      if (!isChunkLoadedAt(wx, wz))
        continue;
      if (isUnderwater(wx, wz))
        continue;
      int cx = (int)std::floor(wx / ws);
      int cz = (int)std::floor(wz / ws);
      auto itChunk = mChunks.find({cx, cz});
      if (itChunk == mChunks.end())
        continue;
      ChunkData &cd = itChunk->second;
      float wy = getHeightAt(wx, wz);
      glm::vec3 pos(wx, wy, wz);
      glm::vec3 scale(1.0f);
      glm::vec3 rot(0.0f);
      size_t idx = addPrefabInstance(prefabName, pos, scale, rot, cd);
      if (idx == std::numeric_limits<size_t>::max())
        continue;
      auto itInst =
          reg.has<InstancedMeshComponent>(itPrefab->second.entity)
              ? &reg.get<InstancedMeshComponent>(itPrefab->second.entity)
              : nullptr;
      if (itInst && idx < itInst->instanceTransforms.size()) {
        mPaintedInstances[{cx, cz}]
            .prefabMatrices[prefabName]
            .push_back(itInst->instanceTransforms[idx]);
      }
      if (prefabName == "prefab_pine") {
        const size_t chunkSlot = cd.prefabInstanceMatrices[prefabName].empty()
                                     ? 0
                                     : cd.prefabInstanceMatrices[prefabName].size() - 1;
        registerTreeInstance(prefabName, idx, chunkSlot, pos, scale, cx, cz,
                             &cd);
        cd.treeCount++;
        mStats.totalTreeEntities++;
      } else if (prefabName == "prefab_rock") {
        cd.rockCount++;
        mStats.totalRockEntities++;
      }
      changed = true;
    }
  }

  if (changed)
    rebuildPrefabInstances(prefabName);
  return changed;
}

// ═══════════════════════════════════════════════════════════════
// CHUNK MANAGEMENT
// ═══════════════════════════════════════════════════════════════

std::vector<TerrainSystem::ChunkCoord>
TerrainSystem::getChunksByDistance(int cx, int cz, int radius) const {
  std::vector<ChunkCoord> coords;
  for (int dz = -radius; dz <= radius; ++dz)
    for (int dx = -radius; dx <= radius; ++dx)
      coords.push_back({cx + dx, cz + dz});

  // Sort by distance from camera chunk (nearest first)
  std::sort(coords.begin(), coords.end(),
            [cx, cz](const ChunkCoord &a, const ChunkCoord &b) {
              int da = (a.x - cx) * (a.x - cx) + (a.z - cz) * (a.z - cz);
              int db = (b.x - cx) * (b.x - cx) + (b.z - cz) * (b.z - cz);
              return da < db;
            });
  return coords;
}

void TerrainSystem::update(const glm::vec3 &cameraPos) {
  if (!mSettings.enabled || !mScene)
    return;

  ensureGpuRenderer();

  // Always upload finished chunk jobs, even if the camera stayed in the same
  // chunk this frame.
  flushPendingChunks();

  int cx = (int)std::floor(cameraPos.x / mSettings.chunkWorldSize);
  int cz = (int)std::floor(cameraPos.z / mSettings.chunkWorldSize);

  int collisionBudget = std::max(0, mSettings.collisionUpdatesPerFrame);
  while (collisionBudget > 0 && !mPendingCollisionUpdates.empty()) {
    ChunkCoord coord = mPendingCollisionUpdates.back();
    mPendingCollisionUpdates.pop_back();
    auto it = mChunks.find(coord);
    if (it == mChunks.end())
      continue;
    updateCollisionForChunk(coord.x, coord.z, it->second);
    --collisionBudget;
  }

  ChunkCoord currentChunk = {cx, cz};
  const bool cameraChunkChanged = !(currentChunk == mLastCameraChunk);
  mLastCameraChunk = currentChunk;

  // Get desired chunks sorted by distance (nearest loaded first)
  auto desired = getChunksByDistance(cx, cz, mSettings.viewDistance);
  std::unordered_set<ChunkCoord, ChunkCoordHash> desiredSet;
  desiredSet.reserve(desired.size());
  for (const auto &coord : desired)
    desiredSet.insert(coord);

  if (cameraChunkChanged) {
    // Unload chunks outside view distance only when the anchor moves. This
    // keeps idle streaming cheap while still updating the active window.
    std::vector<ChunkCoord> toUnload;
    std::unordered_set<std::string> prefabsToRebuild;
    for (auto &[coord, data] : mChunks) {
      if (desiredSet.find(coord) == desiredSet.end())
        toUnload.push_back(coord);
    }
    for (auto &coord : toUnload)
      unloadChunk(coord.x, coord.z, &prefabsToRebuild);
    for (const auto &prefabName : prefabsToRebuild)
      rebuildPrefabInstances(prefabName);
  }

  // Load missing chunks async (nearest first due to sorted order). This must
  // run every frame, not only after crossing a chunk boundary; otherwise the
  // first small worker batch finishes and the rest of the view distance never
  // gets queued until the editor camera is dragged into a new chunk.
  const int maxInFlight = std::max(
      1, mSettings.terrainWorkerThreads > 0 ? mSettings.terrainWorkerThreads
                                            : mSettings.maxConcurrentChunkJobs);
  const int currentInFlight = (int)mInFlight.size();
  int availableJobSlots = std::max(0, maxInFlight - currentInFlight);
  int loadsThisUpdate = 0;
  const bool useGpuPath = gpuTerrainActive();
  for (auto &coord : desired) {
    if (availableJobSlots <= 0 ||
        loadsThisUpdate >= std::max(1, mSettings.maxChunkLoadsPerUpdate))
      break;
    if (mChunks.find(coord) == mChunks.end() &&
        mInFlight.find(coord) == mInFlight.end()) {
      loadChunkAsync(coord.x, coord.z, computeChunkLod(cx, cz, coord.x, coord.z),
                     chunkNeedsCollision(mSettings, cx, cz, coord.x, coord.z),
                     useGpuPath);
      availableJobSlots--;
      loadsThisUpdate++;
    }
  }

  if (!cameraChunkChanged) {
    updateInteractiveTreeResidency(cx, cz);
    syncGpuStats();
    return;
  }

  // Existing loaded chunks can move between LOD bands as the camera crosses
  // chunk boundaries. Rebuild render meshes only; collision stays full-res.
  for (auto &[coord, data] : mChunks) {
    const int desiredLod = computeChunkLod(cx, cz, coord.x, coord.z);
    const bool wantsCollision =
        chunkNeedsCollision(mSettings, cx, cz, coord.x, coord.z);
    const bool hasCollision = data.physicsBodyId != 0xFFFFFFFF;
    const bool collisionChanged = wantsCollision != hasCollision;
    if (desiredLod == data.terrainLod && !collisionChanged)
      continue;
    data.terrainLod = desiredLod;
    if (collisionChanged) {
      if (collisionBudget > 0) {
        rebuildChunkTerrain(coord.x, coord.z, true);
        --collisionBudget;
      } else {
        if (std::find(mPendingCollisionUpdates.begin(),
                      mPendingCollisionUpdates.end(),
                      coord) == mPendingCollisionUpdates.end()) {
          mPendingCollisionUpdates.push_back(coord);
        }
        rebuildChunkTerrain(coord.x, coord.z, false);
      }
    } else {
      rebuildChunkTerrain(coord.x, coord.z, false);
    }
  }

  updateInteractiveTreeResidency(cx, cz);
  syncGpuStats();
}

// ═══════════════════════════════════════════════════════════════
// MESH GENERATION
// ═══════════════════════════════════════════════════════════════

std::vector<OBJModel::VertexData> TerrainSystem::generateChunkMesh(int cx,
                                                                   int cz) {
  return generateChunkMeshLod(cx, cz, 0);
}

std::vector<OBJModel::VertexData> TerrainSystem::generateChunkMeshLod(int cx,
                                                                      int cz,
                                                                      int lod) {
  int size = lodResolution(lod);
  float worldSize = mSettings.chunkWorldSize;
  float step = worldSize / (float)size;
  float originX = cx * worldSize;
  float originZ = cz * worldSize;

  std::vector<std::vector<float>> heights(size + 1,
                                          std::vector<float>(size + 1));
  std::vector<std::vector<float>> biomeIds(size + 1,
                                           std::vector<float>(size + 1));

  for (int z = 0; z <= size; ++z) {
    for (int x = 0; x <= size; ++x) {
      float wx = originX + x * step;
      float wz = originZ + z * step;
      heights[z][x] = sampleHeight(wx, wz);
      biomeIds[z][x] = (float)(int)getBiome(wx, wz) / 5.0f;
    }
  }

  auto getNormal = [&](int x, int z) -> glm::vec3 {
    float hL = (x > 0) ? heights[z][x - 1] : heights[z][x];
    float hR = (x < size) ? heights[z][x + 1] : heights[z][x];
    float hD = (z > 0) ? heights[z - 1][x] : heights[z][x];
    float hU = (z < size) ? heights[z + 1][x] : heights[z][x];
    return glm::normalize(glm::vec3(-(hR - hL), 2.0f * step, -(hU - hD)));
  };

  std::vector<OBJModel::VertexData> verts;
  verts.reserve(size * size * 6);
  float uvScale = 1.0f / (float)size;

  for (int z = 0; z < size; ++z) {
    for (int x = 0; x < size; ++x) {
      float wx0 = originX + x * step;
      float wx1 = originX + (x + 1) * step;
      float wz0 = originZ + z * step;
      float wz1 = originZ + (z + 1) * step;

      glm::vec3 p00(wx0, heights[z][x], wz0);
      glm::vec3 p10(wx1, heights[z][x + 1], wz0);
      glm::vec3 p01(wx0, heights[z + 1][x], wz1);
      glm::vec3 p11(wx1, heights[z + 1][x + 1], wz1);

      glm::vec3 n00 = getNormal(x, z);
      glm::vec3 n10 = getNormal(x + 1, z);
      glm::vec3 n01 = getNormal(x, z + 1);
      glm::vec3 n11 = getNormal(x + 1, z + 1);

      glm::vec2 uv00(x * uvScale, biomeIds[z][x]);
      glm::vec2 uv10((x + 1) * uvScale, biomeIds[z][x + 1]);
      glm::vec2 uv01(x * uvScale, biomeIds[z + 1][x]);
      glm::vec2 uv11((x + 1) * uvScale, biomeIds[z + 1][x + 1]);

      verts.push_back({p00, uv00, n00});
      verts.push_back({p01, uv01, n01});
      verts.push_back({p10, uv10, n10});
      verts.push_back({p10, uv10, n10});
      verts.push_back({p01, uv01, n01});
      verts.push_back({p11, uv11, n11});
    }
  }

  mStats.verticesGenerated += (int)verts.size();
  mStats.trianglesGenerated += (int)verts.size() / 3;
  return verts;
}

int TerrainSystem::computeChunkLod(int cameraChunkX, int cameraChunkZ,
                                   int chunkX, int chunkZ) const {
  const int dist = std::max(std::abs(chunkX - cameraChunkX),
                            std::abs(chunkZ - cameraChunkZ));
  if (dist <= 1)
    return 0;
  if (dist <= 3)
    return 1;
  if (dist <= 6)
    return 2;
  if (dist <= 12)
    return 3;
  return 4;
}

int TerrainSystem::lodResolution(int lod) const {
  const int base = std::max(4, mSettings.chunkSize);
  const int shift = std::clamp(lod, 0, 4);
  return std::max(4, base >> shift);
}

// ═══════════════════════════════════════════════════════════════
// WATER PLANE GENERATION
// ═══════════════════════════════════════════════════════════════

std::vector<OBJModel::VertexData> TerrainSystem::generateWaterPlane(int cx,
                                                                    int cz) {
  float ws = mSettings.chunkWorldSize;
  float ox = cx * ws;
  float oz = cz * ws;
  float step = ws / (float)WATER_RESOLUTION;

  std::vector<OBJModel::VertexData> verts;
  verts.reserve(WATER_RESOLUTION * WATER_RESOLUTION * 6);

  for (int z = 0; z < WATER_RESOLUTION; ++z) {
    for (int x = 0; x < WATER_RESOLUTION; ++x) {
      float x0 = ox + x * step;
      float x1 = ox + (x + 1) * step;
      float z0 = oz + z * step;
      float z1 = oz + (z + 1) * step;
      float y = mSettings.seaLevel;

      glm::vec3 p00(x0, y, z0), p10(x1, y, z0);
      glm::vec3 p01(x0, y, z1), p11(x1, y, z1);
      glm::vec3 n(0, 1, 0);
      glm::vec2 uv(0.0f, 0.0f); // Ocean biome = 0/5

      verts.push_back({p00, uv, n});
      verts.push_back({p01, uv, n});
      verts.push_back({p10, uv, n});
      verts.push_back({p10, uv, n});
      verts.push_back({p01, uv, n});
      verts.push_back({p11, uv, n});
    }
  }
  return verts;
}

// ═══════════════════════════════════════════════════════════════
// ENTITY HELPER
// ═══════════════════════════════════════════════════════════════

void TerrainSystem::clearPrefabs() {
  for (auto &[_, pd] : mPrefabs) {
    if (mScene && pd.entity != 0 &&
        mScene->registry().has<LifecycleComponent>(pd.entity)) {
      mScene->deleteEntity(pd.entity);
    }
    if (pd.runtimeAsset && mAssets && !pd.assetId.empty()) {
      mAssets->releaseOBJ(pd.assetId);
    }
  }
  mPrefabs.clear();
  mPrefabInstanceEntities.clear();
}

void TerrainSystem::addPrefabFromVerts(
    const std::string &name, const std::vector<OBJModel::VertexData> &verts) {
  if (mPrefabs.find(name) != mPrefabs.end())
    return;
  if (!mAssets)
    return;

  auto model = std::make_unique<OBJModel>();
  model->loadFromVertices(verts, name);
  const std::string assetId = "__runtime_prefab_" + name;
  OBJHandle h = mAssets->registerRuntimeOBJ(assetId, std::move(model));
  OBJModel *runtimeModel = mAssets->getOBJ(h);
  if (!h.valid() || !runtimeModel)
    return;

  EntityId eid = mScene->createEmptyEntity(name);
  auto &reg = mScene->registry();
  reg.emplace<TransientComponent>(eid);
  auto &t = reg.get<TransformComponent>(eid);
  t.position = glm::vec3(0.0f);
  t.scale = glm::vec3(1.0f);

  reg.emplace<InstancedMeshComponent>(eid);
  auto &inst = reg.get<InstancedMeshComponent>(eid);
  inst.type = MeshComponent::AssetType::OBJ;
  inst.objModel = runtimeModel;
  inst.objHandle = h;
  inst.useTerrainShading = true;
  inst.visible = true;
  inst.castsShadow = true;
  applyVegetationCullProfile(mSettings, name, inst);

  PrefabData pd;
  pd.entity = eid;
  pd.assetId = assetId;
  pd.runtimeAsset = true;
  mPrefabs[name] = std::move(pd);
}

size_t TerrainSystem::addPrefabInstance(const std::string &name,
                                        const glm::vec3 &pos,
                                        const glm::vec3 &scale,
                                        const glm::vec3 &rot,
                                        ChunkData &chunk) {
  auto it = mPrefabs.find(name);
  if (it == mPrefabs.end())
    return std::numeric_limits<size_t>::max();

  auto &reg = mScene->registry();
  if (!reg.has<InstancedMeshComponent>(it->second.entity))
    return std::numeric_limits<size_t>::max();

  auto &inst = reg.get<InstancedMeshComponent>(it->second.entity);
  const PrefabData &pd = it->second;

  // Final scale = caller scale * per-prefab autoScale
  glm::vec3 finalScale = scale * pd.autoScale;

  glm::mat4 m(1.0f);
  m = glm::translate(m, pos);
  // Apply caller rotation then prefab base rotation fix
  m = glm::rotate(m, rot.y, glm::vec3(0, 1, 0));
  m = glm::rotate(m, rot.x, glm::vec3(1, 0, 0));
  m = glm::rotate(m, rot.z, glm::vec3(0, 0, 1));
  // Base rotation to correct axis (e.g. Z-up OBJ → Y-up)
  m = glm::rotate(m, pd.baseRot.x, glm::vec3(1, 0, 0));
  m = glm::rotate(m, pd.baseRot.y, glm::vec3(0, 1, 0));
  m = glm::rotate(m, pd.baseRot.z, glm::vec3(0, 0, 1));
  m = glm::scale(m, finalScale);

  inst.instanceTransforms.push_back(m);
  const size_t globalIndex = inst.instanceTransforms.size() - 1;
  inst.isDirty = true;

  chunk.prefabInstanceCounts[name]++;
  chunk.prefabInstanceMatrices[name].push_back(m);
  chunk.prefabInstanceGlobalIndices[name].push_back(
      static_cast<uint32_t>(globalIndex));
  chunk.prefabInstanceEntities[name].push_back(0);
  return globalIndex;
}

void TerrainSystem::registerTreeInstance(const std::string &prefabName,
                                         size_t instanceIndex,
                                         size_t chunkInstanceSlot,
                                         const glm::vec3 &pos,
                                         const glm::vec3 &scale, int cx,
                                         int cz, ChunkData *chunk) {
  if (!mScene)
    return;

  auto &reg = mScene->registry();
  EntityId eid = mScene->createEmptyEntity("Tree");
  reg.emplace<TransientComponent>(eid);
  reg.emplace<NameComponent>(eid, "Tree");

  auto &tr = reg.get<TransformComponent>(eid);
  tr.position = pos;
  tr.rotation = glm::vec3(0.0f);
  tr.scale = glm::vec3(1.0f);

  auto &rb = reg.emplace<RigidbodyComponent>(eid);
  rb.type = RigidbodyComponent::Type::Static;

  auto &col = reg.emplace<ColliderComponent>(eid);
  col.shape = ColliderComponent::Shape::Capsule;

  glm::vec3 bMin(-0.5f), bMax(0.5f);
  glm::vec3 center(0.0f);
  glm::vec3 extents(1.0f);

  auto itPrefab = mPrefabs.find(prefabName);
  if (itPrefab != mPrefabs.end() &&
      reg.has<InstancedMeshComponent>(itPrefab->second.entity)) {
    const auto &inst =
        reg.get<InstancedMeshComponent>(itPrefab->second.entity);
    bool hasBounds = false;
    if (inst.type == MeshComponent::AssetType::OBJ && inst.objModel) {
      hasBounds = inst.objModel->getGlobalBounds(bMin, bMax);
    } else if (inst.type == MeshComponent::AssetType::FBX && inst.ufbxModel) {
      hasBounds = inst.ufbxModel->getGlobalBounds(bMin, bMax);
    }
    if (hasBounds) {
      center = (bMin + bMax) * 0.5f;
      extents = (bMax - bMin);
    }
  }

  glm::vec3 finalScale = scale;
  if (itPrefab != mPrefabs.end()) {
    finalScale *= itPrefab->second.autoScale;
  }

  const float height = std::max(0.5f, extents.y * finalScale.y);
  const float radius =
      std::max(0.15f, 0.25f * std::max(extents.x * finalScale.x,
                                       extents.z * finalScale.z));
  const float cylinderHeight = std::max(0.1f, height - radius * 2.0f);
  col.dimensions = glm::vec3(radius, cylinderHeight, radius);

  tr.position = pos + glm::vec3(0.0f, center.y * finalScale.y, 0.0f);

  auto &tree = reg.emplace<TreeComponent>(eid);
  tree.health = 3.0f;
  tree.instanceIndex = static_cast<uint32_t>(instanceIndex);
  tree.chunkInstanceSlot = static_cast<uint32_t>(chunkInstanceSlot);
  tree.prefabName = prefabName;
  tree.chunkX = cx;
  tree.chunkZ = cz;

  auto &vec = mPrefabInstanceEntities[prefabName];
  if (vec.size() <= instanceIndex)
    vec.resize(instanceIndex + 1, 0);
  vec[instanceIndex] = eid;
  if (chunk) {
    auto &chunkEntities = chunk->prefabInstanceEntities[prefabName];
    if (chunkEntities.size() <= chunkInstanceSlot)
      chunkEntities.resize(chunkInstanceSlot + 1, 0);
    chunkEntities[chunkInstanceSlot] = eid;

    auto &chunkGlobals = chunk->prefabInstanceGlobalIndices[prefabName];
    if (chunkGlobals.size() <= chunkInstanceSlot)
      chunkGlobals.resize(chunkInstanceSlot + 1,
                          static_cast<uint32_t>(instanceIndex));
    chunkGlobals[chunkInstanceSlot] = static_cast<uint32_t>(instanceIndex);
  }
}

void TerrainSystem::removeLastPrefabInstances(const std::string &prefabName,
                                              size_t count) {
  if (!mScene || count == 0)
    return;

  auto it = mPrefabInstanceEntities.find(prefabName);
  if (it == mPrefabInstanceEntities.end())
    return;

  auto &vec = it->second;
  auto &reg = mScene->registry();
  size_t removeCount = std::min(count, vec.size());
  for (size_t i = 0; i < removeCount; ++i) {
    uint32_t eid = vec.back();
    vec.pop_back();
    if (eid == 0)
      continue;
    if (mPhysicsSystem && reg.has<RigidbodyComponent>(eid)) {
      auto &rb = reg.get<RigidbodyComponent>(eid);
      mPhysicsSystem->removeBody(rb.bodyID);
    }
    mScene->deleteEntity(eid);
  }
}

void TerrainSystem::demoteInteractiveTreeEntity(EntityId treeEntity) {
  if (!mScene)
    return;

  auto &reg = mScene->registry();
  if (!reg.has<TreeComponent>(treeEntity) || reg.has<MeshComponent>(treeEntity))
    return;

  const TreeComponent tree = reg.get<TreeComponent>(treeEntity);
  if (tree.instanceIndex == INVALID_TREE_INSTANCE_INDEX)
    return;

  auto itMap = mPrefabInstanceEntities.find(tree.prefabName);
  if (itMap != mPrefabInstanceEntities.end() &&
      tree.instanceIndex < itMap->second.size()) {
    itMap->second[tree.instanceIndex] = 0;
  }

  auto itChunk = mChunks.find({tree.chunkX, tree.chunkZ});
  if (itChunk != mChunks.end()) {
    auto &chunk = itChunk->second;
    auto &chunkEntities = chunk.prefabInstanceEntities[tree.prefabName];
    size_t chunkSlot = tree.chunkInstanceSlot;
    if (chunkSlot >= chunkEntities.size() ||
        chunkEntities[chunkSlot] != treeEntity) {
      auto found =
          std::find(chunkEntities.begin(), chunkEntities.end(), treeEntity);
      chunkSlot = (found != chunkEntities.end())
                      ? static_cast<size_t>(std::distance(chunkEntities.begin(),
                                                          found))
                      : std::numeric_limits<size_t>::max();
    }
    if (chunkSlot != std::numeric_limits<size_t>::max())
      chunkEntities[chunkSlot] = 0;
    if (chunk.treeCount > 0)
      chunk.treeCount--;
  }

  if (mStats.totalTreeEntities > 0)
    mStats.totalTreeEntities--;

  if (mPhysicsSystem && reg.has<RigidbodyComponent>(treeEntity)) {
    auto &rb = reg.get<RigidbodyComponent>(treeEntity);
    if (rb.bodyID != 0xFFFFFFFF)
      mPhysicsSystem->removeBody(rb.bodyID);
  }
  mScene->deleteEntity(treeEntity);
}

void TerrainSystem::updateInteractiveTreeResidency(int cameraChunkX,
                                                   int cameraChunkZ) {
  if (!mScene)
    return;

  auto &reg = mScene->registry();
  std::vector<EntityId> toDemote;
  const int chunkRadius = std::max(0, mSettings.interactiveTreeChunkRadius);

  for (auto entity : reg.view<TreeComponent>()) {
    if (!reg.has<TreeComponent>(entity))
      continue;
    const auto &tree = reg.get<TreeComponent>(entity);
    if (!isInteractiveTreePrefab(tree.prefabName) ||
        reg.has<MeshComponent>(entity) ||
        tree.instanceIndex == INVALID_TREE_INSTANCE_INDEX) {
      continue;
    }

    const int dist = std::max(std::abs(tree.chunkX - cameraChunkX),
                              std::abs(tree.chunkZ - cameraChunkZ));
    if (dist > chunkRadius)
      toDemote.push_back(entity);
  }

  for (EntityId entity : toDemote)
    demoteInteractiveTreeEntity(entity);

  for (auto &[coord, chunk] : mChunks) {
    if (std::max(std::abs(coord.x - cameraChunkX), std::abs(coord.z - cameraChunkZ)) >
        chunkRadius) {
      continue;
    }

    int interactiveCount = 0;
    const char *treePrefabs[] = {"prefab_pine", "prefab_oak", "prefab_birch"};
    for (const char *prefabNameC : treePrefabs) {
      const std::string prefabName = prefabNameC;
      auto itMats = chunk.prefabInstanceMatrices.find(prefabName);
      if (itMats == chunk.prefabInstanceMatrices.end())
        continue;

      auto &mats = itMats->second;
      auto &chunkEntities = chunk.prefabInstanceEntities[prefabName];
      auto &chunkGlobals = chunk.prefabInstanceGlobalIndices[prefabName];
      if (chunkEntities.size() < mats.size())
        chunkEntities.resize(mats.size(), 0);
      if (chunkGlobals.size() < mats.size())
        chunkGlobals.resize(mats.size(), INVALID_TREE_INSTANCE_INDEX);

      for (size_t i = 0; i < mats.size(); ++i) {
        const uint32_t eid = chunkEntities[i];
        if (eid == 0)
          continue;
        if (!reg.has<TreeComponent>(eid) || reg.has<MeshComponent>(eid)) {
          auto itMap = mPrefabInstanceEntities.find(prefabName);
          if (itMap != mPrefabInstanceEntities.end() &&
              i < chunkGlobals.size() && chunkGlobals[i] < itMap->second.size()) {
            itMap->second[chunkGlobals[i]] = 0;
          }
          chunkEntities[i] = 0;
          continue;
        }
        auto &tree = reg.get<TreeComponent>(eid);
        if (chunkGlobals[i] != INVALID_TREE_INSTANCE_INDEX)
          tree.instanceIndex = chunkGlobals[i];
        tree.chunkInstanceSlot = static_cast<uint32_t>(i);
        tree.chunkX = coord.x;
        tree.chunkZ = coord.z;
        interactiveCount++;
      }
    }
    chunk.treeCount = interactiveCount;

    for (const char *prefabNameC : treePrefabs) {
      const std::string prefabName = prefabNameC;
      auto itMats = chunk.prefabInstanceMatrices.find(prefabName);
      if (itMats == chunk.prefabInstanceMatrices.end())
        continue;
      auto &mats = itMats->second;
      auto &chunkEntities = chunk.prefabInstanceEntities[prefabName];
      auto &chunkGlobals = chunk.prefabInstanceGlobalIndices[prefabName];
      for (size_t i = 0; i < mats.size(); ++i) {
        if (interactiveCount >=
            std::max(0, mSettings.maxInteractiveTreesPerChunk))
          break;
        if (chunkEntities[i] != 0)
          continue;

        const uint32_t globalIndex = chunkGlobals[i];
        if (globalIndex == INVALID_TREE_INSTANCE_INDEX)
          continue;

        const glm::vec3 pos = glm::vec3(mats[i][3]);
        if (!shouldSpawnInteractiveTree(mSettings, cameraChunkX, cameraChunkZ,
                                        coord.x, coord.z, pos, mTreeNoise,
                                        interactiveCount)) {
          continue;
        }

        glm::vec3 scale(glm::length(glm::vec3(mats[i][0])),
                        glm::length(glm::vec3(mats[i][1])),
                        glm::length(glm::vec3(mats[i][2])));
        registerTreeInstance(prefabName, globalIndex, i, pos, scale, coord.x,
                             coord.z, &chunk);
        chunk.treeCount++;
        mStats.totalTreeEntities++;
        interactiveCount++;
      }
    }
  }
}

bool TerrainSystem::chopTree(EntityId treeEntity) {
  if (!mScene || !mScene->registry().has<TreeComponent>(treeEntity))
    return false;

  auto &reg = mScene->registry();
  auto &tree = reg.get<TreeComponent>(treeEntity);
  tree.health -= 1.0f;
  if (tree.health > 0.0f)
    return false;

  auto itPrefab = mPrefabs.find(tree.prefabName);
  if (itPrefab == mPrefabs.end())
    return false;
  if (!reg.has<InstancedMeshComponent>(itPrefab->second.entity))
    return false;

  auto itChunk = mChunks.find({tree.chunkX, tree.chunkZ});
  if (itChunk != mChunks.end()) {
    auto &cd = itChunk->second;
    auto &mats = cd.prefabInstanceMatrices[tree.prefabName];
    auto &ents = cd.prefabInstanceEntities[tree.prefabName];
    auto &globals = cd.prefabInstanceGlobalIndices[tree.prefabName];
    if (ents.size() < mats.size())
      ents.resize(mats.size(), 0);
    if (globals.size() < mats.size())
      globals.resize(mats.size(), INVALID_TREE_INSTANCE_INDEX);

    size_t chunkSlot = tree.chunkInstanceSlot;
    if (chunkSlot >= mats.size() || ents[chunkSlot] != treeEntity) {
      auto found = std::find(ents.begin(), ents.end(), treeEntity);
      if (found != ents.end()) {
        chunkSlot = static_cast<size_t>(std::distance(ents.begin(), found));
      } else if (tree.instanceIndex != INVALID_TREE_INSTANCE_INDEX) {
        auto gIt = std::find(globals.begin(), globals.end(), tree.instanceIndex);
        if (gIt != globals.end())
          chunkSlot = static_cast<size_t>(std::distance(globals.begin(), gIt));
      }
    }

    if (chunkSlot < mats.size()) {
      mats.erase(mats.begin() + static_cast<std::ptrdiff_t>(chunkSlot));
      ents.erase(ents.begin() + static_cast<std::ptrdiff_t>(chunkSlot));
      globals.erase(globals.begin() + static_cast<std::ptrdiff_t>(chunkSlot));
    }

    auto itCount = cd.prefabInstanceCounts.find(tree.prefabName);
    if (itCount != cd.prefabInstanceCounts.end() && itCount->second > 0)
      itCount->second--;
    if (cd.treeCount > 0)
      cd.treeCount--;
  }
  if (mStats.totalTreeEntities > 0)
    mStats.totalTreeEntities--;

  rebuildPrefabInstances(tree.prefabName);

  if (mPhysicsSystem && reg.has<RigidbodyComponent>(treeEntity)) {
    auto &rb = reg.get<RigidbodyComponent>(treeEntity);
    if (rb.bodyID != 0xFFFFFFFF)
      mPhysicsSystem->removeBody(rb.bodyID);
  }
  mScene->deleteEntity(treeEntity);
  return true;
}

bool TerrainSystem::movePrefabInstance(const std::string &prefabName,
                                       size_t instanceIndex,
                                       const glm::vec3 &delta) {
  if (!mScene)
    return false;
  auto it = mPrefabs.find(prefabName);
  if (it == mPrefabs.end())
    return false;
  auto &reg = mScene->registry();
  if (!reg.has<InstancedMeshComponent>(it->second.entity))
    return false;
  auto &inst = reg.get<InstancedMeshComponent>(it->second.entity);
  if (instanceIndex >= inst.instanceTransforms.size())
    return false;
  inst.instanceTransforms[instanceIndex][3] += glm::vec4(delta, 0.0f);
  for (auto &[coord, chunk] : mChunks) {
    auto itGlobals = chunk.prefabInstanceGlobalIndices.find(prefabName);
    auto itMats = chunk.prefabInstanceMatrices.find(prefabName);
    if (itGlobals == chunk.prefabInstanceGlobalIndices.end() ||
        itMats == chunk.prefabInstanceMatrices.end()) {
      continue;
    }

    auto &globals = itGlobals->second;
    auto &mats = itMats->second;
    for (size_t i = 0; i < globals.size() && i < mats.size(); ++i) {
      if (globals[i] == instanceIndex) {
        mats[i] = inst.instanceTransforms[instanceIndex];
        break;
      }
    }
  }
  inst.isDirty = true;
  return true;
}

bool TerrainSystem::getPrefabInstanceMatrix(const std::string &prefabName,
                                            size_t instanceIndex,
                                            glm::mat4 &out) const {
  auto it = mPrefabs.find(prefabName);
  if (it == mPrefabs.end())
    return false;
  if (!mScene)
    return false;
  auto &reg = mScene->registry();
  if (!reg.has<InstancedMeshComponent>(it->second.entity))
    return false;
  auto &inst = reg.get<InstancedMeshComponent>(it->second.entity);
  if (instanceIndex >= inst.instanceTransforms.size())
    return false;
  out = inst.instanceTransforms[instanceIndex];
  return true;
}

bool TerrainSystem::setPrefabInstanceMatrix(const std::string &prefabName,
                                            size_t instanceIndex,
                                            const glm::mat4 &m) {
  auto it = mPrefabs.find(prefabName);
  if (it == mPrefabs.end())
    return false;
  if (!mScene)
    return false;
  auto &reg = mScene->registry();
  if (!reg.has<InstancedMeshComponent>(it->second.entity))
    return false;
  auto &inst = reg.get<InstancedMeshComponent>(it->second.entity);
  if (instanceIndex >= inst.instanceTransforms.size())
    return false;
  inst.instanceTransforms[instanceIndex] = m;
  for (auto &[coord, chunk] : mChunks) {
    auto itGlobals = chunk.prefabInstanceGlobalIndices.find(prefabName);
    auto itMats = chunk.prefabInstanceMatrices.find(prefabName);
    if (itGlobals == chunk.prefabInstanceGlobalIndices.end() ||
        itMats == chunk.prefabInstanceMatrices.end()) {
      continue;
    }

    auto &globals = itGlobals->second;
    auto &mats = itMats->second;
    for (size_t i = 0; i < globals.size() && i < mats.size(); ++i) {
      if (globals[i] == instanceIndex) {
        mats[i] = m;
        break;
      }
    }
  }
  inst.isDirty = true;
  return true;
}

bool TerrainSystem::convertTreeToEntity(EntityId treeEntity) {
  if (!mScene || !mScene->registry().has<TreeComponent>(treeEntity))
    return false;

  auto &reg = mScene->registry();
  auto &tree = reg.get<TreeComponent>(treeEntity);
  auto itPrefab = mPrefabs.find(tree.prefabName);
  if (itPrefab == mPrefabs.end())
    return false;
  if (!reg.has<InstancedMeshComponent>(itPrefab->second.entity))
    return false;

  auto &inst = reg.get<InstancedMeshComponent>(itPrefab->second.entity);
  glm::mat4 instM(1.0f);
  if (tree.instanceIndex < inst.instanceTransforms.size())
    instM = inst.instanceTransforms[tree.instanceIndex];

  auto itChunk = mChunks.find({tree.chunkX, tree.chunkZ});
  if (itChunk != mChunks.end()) {
    auto &cd = itChunk->second;
    auto &mats = cd.prefabInstanceMatrices[tree.prefabName];
    auto &ents = cd.prefabInstanceEntities[tree.prefabName];
    auto &globals = cd.prefabInstanceGlobalIndices[tree.prefabName];
    if (ents.size() < mats.size())
      ents.resize(mats.size(), 0);
    if (globals.size() < mats.size())
      globals.resize(mats.size(), INVALID_TREE_INSTANCE_INDEX);

    size_t chunkSlot = tree.chunkInstanceSlot;
    if (chunkSlot >= mats.size() || ents[chunkSlot] != treeEntity) {
      auto found = std::find(ents.begin(), ents.end(), treeEntity);
      if (found != ents.end()) {
        chunkSlot = static_cast<size_t>(std::distance(ents.begin(), found));
      } else if (tree.instanceIndex != INVALID_TREE_INSTANCE_INDEX) {
        auto gIt = std::find(globals.begin(), globals.end(), tree.instanceIndex);
        if (gIt != globals.end())
          chunkSlot = static_cast<size_t>(std::distance(globals.begin(), gIt));
      }
    }

    if (chunkSlot < mats.size()) {
      mats.erase(mats.begin() + static_cast<std::ptrdiff_t>(chunkSlot));
      ents.erase(ents.begin() + static_cast<std::ptrdiff_t>(chunkSlot));
      globals.erase(globals.begin() + static_cast<std::ptrdiff_t>(chunkSlot));
    }

    auto itCount = cd.prefabInstanceCounts.find(tree.prefabName);
    if (itCount != cd.prefabInstanceCounts.end() && itCount->second > 0)
      itCount->second--;
    if (cd.treeCount > 0)
      cd.treeCount--;
  }
  if (mStats.totalTreeEntities > 0)
    mStats.totalTreeEntities--;

  rebuildPrefabInstances(tree.prefabName);

  // Update transform from instance matrix so physics matches render.
  {
    auto &tr = reg.get<TransformComponent>(treeEntity);
    glm::vec3 skew;
    glm::vec4 persp;
    glm::quat rot;
    glm::vec3 scale;
    glm::vec3 translation = glm::vec3(instM[3]);
    if (glm::decompose(instM, scale, rot, translation, skew, persp)) {
      tr.position = translation;
      tr.rotation = glm::degrees(glm::eulerAngles(rot));
      tr.scale = scale;
    } else {
      tr.position = translation;
    }
  }

  // Add renderable mesh to the tree entity (un-instanced).
  if (!reg.has<MeshComponent>(treeEntity)) {
    if (inst.type == MeshComponent::AssetType::OBJ && inst.objModel) {
      auto &mc = reg.emplace<MeshComponent>(treeEntity, inst.objModel);
      mc.assetId = itPrefab->second.assetId;
    } else if (inst.type == MeshComponent::AssetType::FBX && inst.ufbxModel) {
      auto &mc = reg.emplace<MeshComponent>(treeEntity, inst.ufbxModel);
      mc.assetId = itPrefab->second.assetId;
    }
  }
  tree.instanceIndex = INVALID_TREE_INSTANCE_INDEX;
  tree.chunkInstanceSlot = INVALID_TREE_INSTANCE_INDEX;
  return true;
}

void TerrainSystem::initPrefabs() {
  // Purge dead entities (e.g. after a Scene::clear() from loading a snapshot)
  if (mScene) {
    for (auto it = mPrefabs.begin(); it != mPrefabs.end();) {
      if (!mScene->registry().has<LifecycleComponent>(it->second.entity)) {
        if (it->second.runtimeAsset && mAssets && !it->second.assetId.empty()) {
          mAssets->releaseOBJ(it->second.assetId);
        }
        it = mPrefabs.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Forcefully discard customizable prefabs so path changes via UI apply.
  const char *customizablePrefabs[] = {"prefab_pine",   "prefab_rock",
                                       "prefab_grass",  "prefab_flower",
                                       "prefab_cactus", "prefab_deadtree"};
  for (const char *prefabName : customizablePrefabs) {
    auto it = mPrefabs.find(prefabName);
    if (it == mPrefabs.end())
      continue;
    if (mScene &&
        mScene->registry().has<LifecycleComponent>(it->second.entity)) {
      mScene->deleteEntity(it->second.entity);
    }
    if (it->second.runtimeAsset && mAssets && !it->second.assetId.empty()) {
      mAssets->releaseOBJ(it->second.assetId);
    }
    mPrefabs.erase(it);
  }

  // Generate Prefab geometry once
  std::vector<OBJModel::VertexData> verts;

  auto tryLoadCustomPrefab = [&](const std::string &prefabName,
                                 const std::string &path,
                                 float targetHeight) -> bool {
    if (path.empty() || !mAssets || !mScene)
      return false;

    std::string lowerPath = path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    const bool isFbx = lowerPath.size() >= 4 &&
                       lowerPath.compare(lowerPath.size() - 4, 4, ".fbx") == 0;
    const bool isObj = lowerPath.size() >= 4 &&
                       lowerPath.compare(lowerPath.size() - 4, 4, ".obj") == 0;
    if (!isFbx && !isObj)
      return false;

    auto &reg = mScene->registry();

    if (isFbx) {
      auto handle = mAssets->loadUFBX(path);
      if (!handle.valid())
        return false;
      auto *fbModel = mAssets->getUFBX(handle);
      if (!fbModel || fbModel->submeshCount() == 0)
        return false;

      float autoScale = 1.0f;
      if (targetHeight > 0.0f) {
        glm::vec3 bMin, bMax;
        if (fbModel->getGlobalBounds(bMin, bMax)) {
          const float h = bMax.y - bMin.y;
          if (h > 0.001f)
            autoScale = targetHeight / h;
        }
      }

      EntityId eid = mScene->createEmptyEntity(prefabName);
      reg.emplace<TransientComponent>(eid);
      auto &t = reg.get<TransformComponent>(eid);
      t.position = glm::vec3(0.0f);
      t.scale = glm::vec3(1.0f);
      reg.emplace<InstancedMeshComponent>(eid);
      auto &inst = reg.get<InstancedMeshComponent>(eid);
      inst.type = MeshComponent::AssetType::FBX;
      inst.ufbxModel = fbModel;
      inst.ufbxHandle = handle;
      inst.useTerrainShading = false;
      inst.visible = true;
      inst.castsShadow = true;
      applyVegetationCullProfile(mSettings, prefabName, inst);

      PrefabData pd;
      pd.entity = eid;
      pd.assetId = path;
      pd.runtimeAsset = false;
      pd.autoScale = autoScale;
      mPrefabs[prefabName] = std::move(pd);
      return true;
    }

    auto handle = mAssets->loadOBJ(path);
    if (!handle.valid())
      return false;
    auto *obModel = mAssets->getOBJ(handle);
    if (!obModel || obModel->submeshCount() == 0)
      return false;

    float autoScale = 1.0f;
    glm::vec3 baseRot(0.0f);
    glm::vec3 bMinBefore, bMaxBefore;
    if (obModel->getGlobalBounds(bMinBefore, bMaxBefore)) {
      const glm::vec3 ext = bMaxBefore - bMinBefore;
      OBJModel::UpAxis upAxis = OBJModel::UpAxis::Y;

      // Aggressive auto up-axis detection is useful for trees.
      const bool autoDetectUpAxis = (prefabName == "prefab_pine");
      if (autoDetectUpAxis) {
        const float y = std::max(ext.y, 0.0001f);
        if (ext.x > y * 1.20f && ext.x > ext.z) {
          upAxis = OBJModel::UpAxis::X;
          baseRot = glm::vec3(0.0f, 0.0f, glm::half_pi<float>());
        } else if (ext.z > y * 1.20f && ext.z >= ext.x) {
          upAxis = OBJModel::UpAxis::Z;
          baseRot = glm::vec3(-glm::half_pi<float>(), 0.0f, 0.0f);
        }
      } else if (prefabName == "prefab_grass") {
        // Grass assets are frequently exported with non-Y up axes.
        const float maxYZ = std::max(ext.y, ext.z);
        if (ext.x > maxYZ * 1.08f) {
          upAxis = OBJModel::UpAxis::X;
          baseRot = glm::vec3(0.0f, 0.0f, -glm::half_pi<float>());
        } else {
          const float maxXY = std::max(ext.x, ext.y);
          if (ext.z > maxXY * 1.08f) {
            upAxis = OBJModel::UpAxis::Z;
            baseRot = glm::vec3(-glm::half_pi<float>(), 0.0f, 0.0f);
          }
        }
        if (mSettings.flipCustomGrass) {
          baseRot.x += glm::pi<float>();
        }
      }

      // Ground the model on its source up-axis before any axis-fix rotation.
      obModel->centerAtOrigin(upAxis);

      if (targetHeight > 0.0f) {
        glm::vec3 bMin, bMax;
        if (obModel->getGlobalBounds(bMin, bMax)) {
          float modelHeight = bMax.y - bMin.y;
          if (upAxis == OBJModel::UpAxis::Z)
            modelHeight = bMax.z - bMin.z;
          else if (upAxis == OBJModel::UpAxis::X)
            modelHeight = bMax.x - bMin.x;
          if (modelHeight > 0.001f)
            autoScale = targetHeight / modelHeight;
        }
      }
    }

    EntityId eid = mScene->createEmptyEntity(prefabName);
    reg.emplace<TransientComponent>(eid);
    auto &t = reg.get<TransformComponent>(eid);
    t.position = glm::vec3(0.0f);
    t.scale = glm::vec3(1.0f);
    reg.emplace<InstancedMeshComponent>(eid);
    auto &inst = reg.get<InstancedMeshComponent>(eid);
    inst.type = MeshComponent::AssetType::OBJ;
    inst.objModel = obModel;
    inst.objHandle = handle;
    inst.useTerrainShading = false;
    inst.visible = true;
    inst.castsShadow = true;
    applyVegetationCullProfile(mSettings, prefabName, inst);

    PrefabData pd;
    pd.entity = eid;
    pd.assetId = path;
    pd.runtimeAsset = false;
    pd.autoScale = autoScale;
    pd.baseRot = baseRot;
    mPrefabs[prefabName] = std::move(pd);
    return true;
  };

  (void)tryLoadCustomPrefab("prefab_pine", mSettings.customTreeModelPath,
                            10.0f);
  (void)tryLoadCustomPrefab("prefab_rock", mSettings.customRockModelPath, 2.0f);
  (void)tryLoadCustomPrefab("prefab_grass", mSettings.customGrassModelPath,
                            0.8f);
  (void)tryLoadCustomPrefab("prefab_flower", mSettings.customFlowerModelPath,
                            1.0f);
  (void)tryLoadCustomPrefab("prefab_cactus", mSettings.customCactusModelPath,
                            3.0f);
  (void)tryLoadCustomPrefab("prefab_deadtree",
                            mSettings.customDeadTreeModelPath, 3.0f);

  // Pine Fallback Process
  if (mPrefabs.find("prefab_pine") == mPrefabs.end()) {
    addPineTree(verts);
    addPrefabFromVerts("prefab_pine", verts);
    verts.clear();
  }

  // Oak
  addOakTree(verts);
  addPrefabFromVerts("prefab_oak", verts);
  verts.clear();

  // Birch
  addBirchTree(verts);
  addPrefabFromVerts("prefab_birch", verts);
  verts.clear();

  // Desert Cactus
  addCylinder(verts, {0, 0, 0}, 0.3f, 0.3f, 3.0f, CYLINDER_SEGMENTS, 0.6f);
  addSphere(verts, {0, 3.0f, 0}, 0.3f, SPHERE_RINGS, SPHERE_SECTORS, 0.6f);
  // Arm 1
  addBox(verts, {0.3f, 1.5f, -0.1f}, {1.0f, 1.8f, 0.1f}, 0.6f);
  addCylinder(verts, {0.85f, 1.8f, 0}, 0.15f, 0.15f, 1.0f, CYLINDER_SEGMENTS,
              0.6f);
  addSphere(verts, {0.85f, 2.8f, 0}, 0.15f, SPHERE_RINGS, SPHERE_SECTORS, 0.6f);
  addPrefabFromVerts("prefab_cactus", verts);
  verts.clear();

  // Boulder Rock — only if no custom rock was loaded
  if (mPrefabs.find("prefab_rock") == mPrefabs.end()) {
    addSphere(verts, {0, 0, 0}, 1.0f, SPHERE_RINGS, SPHERE_SECTORS, 0.8f);
    addPrefabFromVerts("prefab_rock", verts);
    verts.clear();
  }

  // Dead Tree — only if no custom dead tree was loaded
  if (mPrefabs.find("prefab_deadtree") == mPrefabs.end()) {
    addDeadTree(verts);
    addPrefabFromVerts("prefab_deadtree", verts);
    verts.clear();
  }

  // Grass Cluster — only if no custom grass was loaded
  if (mPrefabs.find("prefab_grass") == mPrefabs.end()) {
    addGrassCluster(verts);
    addPrefabFromVerts("prefab_grass", verts);
    verts.clear();
  }

  // Bush — low-poly rounded shrub
  if (mPrefabs.find("prefab_bush") == mPrefabs.end()) {
    addSphere(verts, {0, 0.1f, 0}, 0.6f, SPHERE_RINGS, SPHERE_SECTORS, 0.25f);
    addSphere(verts, {0.5f, 0.0f, 0.2f}, 0.4f, SPHERE_RINGS, SPHERE_SECTORS,
              0.25f);
    addSphere(verts, {-0.4f, 0.05f, -0.3f}, 0.35f, SPHERE_RINGS,
              SPHERE_SECTORS, 0.25f);
    addPrefabFromVerts("prefab_bush", verts);
    verts.clear();
  }

  // Flower — only if no custom flower was loaded
  if (mPrefabs.find("prefab_flower") == mPrefabs.end()) {
    addFlower(verts);
    addPrefabFromVerts("prefab_flower", verts);
    verts.clear();
  }
}

// ═══════════════════════════════════════════════════════════════
// VEGETATION SPAWNING — FOREST TREES (Pine, Oak, Birch)
// ═══════════════════════════════════════════════════════════════

void TerrainSystem::spawnTreesForest(int cx, int cz, ChunkData &chunk) {
  float ws = mSettings.chunkWorldSize;
  float ox = cx * ws;
  float oz = cz * ws;
  float spacing = 5.0f;
  int grid = (int)(ws / spacing);
  float biomeUV = 0.4f; // Forest = 2/5
  int interactiveTreeCount = 0;
  const int cameraChunkX =
      (mLastCameraChunk.x == INT_MAX) ? cx : mLastCameraChunk.x;
  const int cameraChunkZ =
      (mLastCameraChunk.z == INT_MAX) ? cz : mLastCameraChunk.z;

  for (int gz = 0; gz < grid; ++gz) {
    for (int gx = 0; gx < grid; ++gx) {
      float wx = ox + (gx + 0.5f) * spacing;
      float wz = oz + (gz + 0.5f) * spacing;

      BiomeType biome = getBiome(wx, wz);
      if (biome != BiomeType::Forest &&
          !(mSettings.singleBiomeOnly && biome == BiomeType::Plains))
        continue;

      float treeVal = mTreeNoise.noise(wx * 0.3f, wz * 0.3f) * 0.5f + 0.5f;
      if (treeVal > mSettings.treeDensity)
        continue;

      // Jitter position
      float jx = mTreeNoise.noise(wx * 1.7f, wz * 2.3f) * spacing * 0.35f;
      float jz =
          mTreeNoise.noise(wx * 2.1f + 50.0f, wz * 1.9f) * spacing * 0.35f;
      wx += jx;
      wz += jz;

      float groundY = sampleHeight(wx, wz);
      if (groundY < mSettings.seaLevel)
        continue;

      TreeType type = chooseForestTreeType(mTreeNoise, wx, wz);
      const std::string prefabName = treePrefabName(type);
      float sizeVar = treeVal;

      // Random Y rotation for natural variety; occasional tilt for realism.
      float rotY = mTreeNoise.noise(wx * 1.1f, wz * 1.1f) * TWO_PI;
      float tiltMask =
          mTreeNoise.noise(wx * 0.9f + 11.0f, wz * 0.9f - 22.0f) * 0.5f + 0.5f;
      float tiltX = 0.0f;
      float tiltZ = 0.0f;
      if (tiltMask > 0.78f) {
        float tiltAmt =
            2.0f + (tiltMask - 0.78f) * (8.0f / 0.22f); // 2..10 deg
        tiltX = mTreeNoise.noise(wx * 5.3f + 100.0f, wz * 4.7f) * tiltAmt;
        tiltZ = mTreeNoise.noise(wx * 4.1f + 200.0f, wz * 5.9f) * tiltAmt;
      }
      glm::vec3 treeRot(tiltX, rotY, tiltZ);

      glm::vec3 treeScale(0.95f + sizeVar * 0.85f);
      if (type == TreeType::Oak)
        treeScale = glm::vec3(1.05f + sizeVar * 0.65f);
      else if (type == TreeType::Birch)
        treeScale = glm::vec3(0.85f + sizeVar * 0.55f,
                              1.05f + sizeVar * 0.80f,
                              0.85f + sizeVar * 0.55f);

      size_t idx = addPrefabInstance(prefabName, {wx, groundY, wz}, treeScale,
                                     treeRot, chunk);
      if (isInteractiveTreePrefab(prefabName)) {
        if (idx != std::numeric_limits<size_t>::max() &&
            shouldSpawnInteractiveTree(mSettings, cameraChunkX, cameraChunkZ,
                                       cx, cz, glm::vec3(wx, groundY, wz),
                                       mTreeNoise, interactiveTreeCount)) {
          const size_t chunkSlot = chunk.prefabInstanceMatrices[prefabName]
                                       .empty()
                                       ? 0
                                       : chunk.prefabInstanceMatrices[prefabName]
                                                 .size() -
                                             1;
          registerTreeInstance(prefabName, idx, chunkSlot,
                               glm::vec3(wx, groundY, wz), treeScale, cx, cz,
                               &chunk);
          chunk.treeCount++;
          mStats.totalTreeEntities++;
          interactiveTreeCount++;
        }
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════
// VEGETATION SPAWNING — DESERT CACTI
// ═══════════════════════════════════════════════════════════════

void TerrainSystem::spawnDesertCacti(int cx, int cz, ChunkData &chunk) {
  float ws = mSettings.chunkWorldSize;
  float ox = cx * ws;
  float oz = cz * ws;
  float spacing = 12.0f; // Cacti are sparse
  int grid = (int)(ws / spacing);
  float biomeUV = 0.6f; // Desert = 3/5

  for (int gz = 0; gz < grid; ++gz) {
    for (int gx = 0; gx < grid; ++gx) {
      float wx = ox + (gx + 0.5f) * spacing;
      float wz = oz + (gz + 0.5f) * spacing;

      if (getBiome(wx, wz) != BiomeType::Desert)
        continue;

      float val = mTreeNoise.noise(wx * 0.2f + 200.0f, wz * 0.2f) * 0.5f + 0.5f;
      if (val > mSettings.treeDensity * 0.6f)
        continue;

      float jx = mTreeNoise.noise(wx * 1.3f, wz * 1.7f) * spacing * 0.3f;
      float jz =
          mTreeNoise.noise(wx * 1.9f + 80.0f, wz * 1.3f) * spacing * 0.3f;
      wx += jx;
      wz += jz;

      float groundY = sampleHeight(wx, wz);
      if (groundY < mSettings.seaLevel)
        continue;

      // Dense grass clusters
      if (val > 0.4f && val < 0.85f) {
        float scale = 0.50f + mDetailNoise.noise(wx * 5.0f, wz * 5.0f) * 0.65f;
        float rotY = mDetailNoise.noise(wx * 1.7f + 44.0f, wz * 1.9f) * TWO_PI;
        addPrefabInstance("prefab_grass", {wx, groundY, wz}, glm::vec3(scale),
                          glm::vec3(0.0f, rotY, 0.0f), chunk);
      }
      // Sparse flowers
      else if (val >= 0.85f) {
        float fScale = 0.8f + mDetailNoise.noise(wx * 8.0f, wz * 8.0f) * 0.5f;
        addPrefabInstance("prefab_flower", {wx, groundY, wz}, glm::vec3(fScale),
                          glm::vec3(0), chunk);
      } else {
        float scaleXZ = 0.8f + val * 0.5f;
        float scaleY = 0.8f + val * 1.5f;
        float rotY = mTreeNoise.noise(wx * 0.5f, wz * 0.5f) * TWO_PI;
        addPrefabInstance("prefab_cactus", {wx, groundY, wz},
                          glm::vec3(scaleXZ, scaleY, scaleXZ),
                          glm::vec3(0, rotY, 0), chunk);
      }

    }
  }
}

// ═══════════════════════════════════════════════════════════════
// VEGETATION SPAWNING — TUNDRA DEAD TREES & ROCKS
// ══════════════════════════════════════════════════════════════m

void TerrainSystem::spawnTundraDecor(int cx, int cz, ChunkData &chunk) {
  float ws = mSettings.chunkWorldSize;
  float ox = cx * ws;
  float oz = cz * ws;
  float biomeUV = 1.0f; // Tundra = 5/5

  // Dead trees — sparse, leafless
  float treeSpacing = 14.0f;
  int treeGrid = (int)(ws / treeSpacing);
  for (int gz = 0; gz < treeGrid; ++gz) {
    for (int gx = 0; gx < treeGrid; ++gx) {
      float wx = ox + (gx + 0.5f) * treeSpacing;
      float wz = oz + (gz + 0.5f) * treeSpacing;

      if (getBiome(wx, wz) != BiomeType::Tundra)
        continue;

      float val =
          mTreeNoise.noise(wx * 0.15f + 300.0f, wz * 0.15f) * 0.5f + 0.5f;
      if (val > mSettings.treeDensity * 0.4f)
        continue;

      float jx = mTreeNoise.noise(wx * 1.5f, wz * 2.0f) * treeSpacing * 0.3f;
      float jz =
          mTreeNoise.noise(wx * 2.0f + 60.0f, wz * 1.5f) * treeSpacing * 0.3f;
      wx += jx;
      wz += jz;

      float groundY = sampleHeight(wx, wz);
      if (groundY < mSettings.seaLevel)
        continue;

      float scale = 0.8f + val * 0.6f;
      float rotY = mTreeNoise.noise(wx * 0.5f, wz * 0.5f) * TWO_PI;
      addPrefabInstance("prefab_deadtree", {wx, groundY, wz}, glm::vec3(scale),
                        glm::vec3(0, rotY, 0), chunk);

    }
  }
}

// ═══════════════════════════════════════════════════════════════
// ROCK/BOULDER SPAWNING — Mountains & Tundra
// ═══════════════════════════════════════════════════════════════

void TerrainSystem::spawnRocksMountain(int cx, int cz, ChunkData &chunk) {
  float ws = mSettings.chunkWorldSize;
  float ox = cx * ws;
  float oz = cz * ws;
  float rockDensity = std::clamp(mSettings.rockDensity, 0.0f, 1.0f);
  float rockScale = std::max(0.1f, mSettings.rockScale);
  if (rockDensity <= 0.0001f)
    return;

  const float clusterSpacing = 13.5f;
  const int clusterGrid = std::max(1, (int)(ws / clusterSpacing));
  const float baseClusterChance = 0.16f + rockDensity * 0.44f;

  auto allowedRockBiome = [](BiomeType b) {
    return b == BiomeType::Mountains || b == BiomeType::Tundra;
  };

  auto trySpawnRock = [&](float wx, float wz, float scaleMul,
                          float buryMul) -> bool {
    BiomeType b = getBiome(wx, wz);
    if (!allowedRockBiome(b))
      return false;

    const float groundY = sampleHeight(wx, wz);
    if (groundY < mSettings.seaLevel)
      return false;

    const float slope = getSlopeAt(wx, wz);
    const float slopeMask = smooth01(10.0f, 28.0f, slope);
    const float biomeBoost =
        (b == BiomeType::Mountains) ? 1.0f
        : (b == BiomeType::Tundra) ? 0.78f
                                   : 0.42f;
    if (slopeMask * biomeBoost < 0.16f)
      return false;

    float sizeJitter =
        0.66f +
        (mRockNoise.noise(wx * 0.9f + 77.0f, wz * 0.9f - 33.0f) * 0.5f + 0.5f) *
            0.55f;
    float sr =
        (0.32f + mRockNoise.noise(wx * 3.0f, wz * 3.0f) * 0.72f) * rockScale *
        sizeJitter * scaleMul * (0.78f + slopeMask * 0.35f);
    sr = std::max(0.12f, sr);

    float rotY = mRockNoise.noise(wx * 0.6f, wz * 0.6f) * TWO_PI;
    float tiltX = mRockNoise.noise(wx * 1.9f + 410.0f, wz * 2.1f - 210.0f) *
                  glm::radians(8.0f);
    float tiltZ = mRockNoise.noise(wx * 2.4f - 150.0f, wz * 1.6f + 90.0f) *
                  glm::radians(6.0f);
    addPrefabInstance("prefab_rock", {wx, groundY - sr * buryMul, wz},
                      glm::vec3(sr * 1.15f, sr * 0.75f, sr * 1.05f),
                      glm::vec3(tiltX, rotY, tiltZ), chunk);
    chunk.rockCount++;
    mStats.totalRockEntities++;
    return true;
  };

  for (int gz = 0; gz < clusterGrid; ++gz) {
    for (int gx = 0; gx < clusterGrid; ++gx) {
      float centerX = ox + (gx + 0.5f) * clusterSpacing;
      float centerZ = oz + (gz + 0.5f) * clusterSpacing;

      float clusterNoise =
          mRockNoise.noise(centerX * 0.12f + 140.0f, centerZ * 0.12f - 80.0f) *
              0.5f +
          0.5f;
      float ridgeNoise =
          mRockNoise.noise(centerX * 0.045f - 600.0f, centerZ * 0.045f + 220.0f) *
              0.5f +
          0.5f;
      float clusterChance = baseClusterChance * (0.72f + ridgeNoise * 0.60f);
      if (clusterNoise > clusterChance)
        continue;

      float jx = mRockNoise.noise(centerX * 0.7f + 100.0f,
                                  centerZ * 0.8f + 35.0f) *
                 clusterSpacing * 0.32f;
      float jz = mRockNoise.noise(centerX * 0.8f - 60.0f,
                                  centerZ * 0.7f + 140.0f) *
                 clusterSpacing * 0.32f;
      centerX += jx;
      centerZ += jz;

      BiomeType centerBiome = getBiome(centerX, centerZ);
      if (!allowedRockBiome(centerBiome))
        continue;

      const float clusterSlope = getSlopeAt(centerX, centerZ);
      const float clusterSlopeMask = smooth01(8.0f, 26.0f, clusterSlope);
      if (centerBiome == BiomeType::Mountains && clusterSlopeMask < 0.15f)
        continue;

      const int clusterCount = 3 + (clusterNoise > 0.70f ? 1 : 0) +
                               (ridgeNoise > 0.64f ? 1 : 0) +
                               (rockDensity > 0.72f ? 1 : 0);
      const float clusterRadius =
          0.9f + clusterNoise * 1.8f + ridgeNoise * 1.1f + rockScale * 0.35f;

      // Anchor rock keeps the cluster grounded without turning into a giant boulder.
      trySpawnRock(centerX, centerZ, 0.72f + ridgeNoise * 0.28f, 0.38f);

      for (int i = 0; i < clusterCount; ++i) {
        float angleSeed =
            mRockNoise.noise(centerX * 0.3f + i * 17.0f, centerZ * 0.3f - i * 11.0f) *
                0.5f +
            0.5f;
        float radiusSeed =
            mRockNoise.noise(centerX * 0.55f - i * 21.0f,
                             centerZ * 0.55f + i * 9.0f) *
                0.5f +
            0.5f;
        float angle = angleSeed * TWO_PI;
        float radius = clusterRadius * (0.28f + radiusSeed * 0.58f);
        float wx = centerX + std::cos(angle) * radius;
        float wz = centerZ + std::sin(angle) * radius;
        float scaleMul = 0.28f + radiusSeed * 0.45f;
        float buryMul = 0.34f + radiusSeed * 0.12f;
        trySpawnRock(wx, wz, scaleMul, buryMul);
      }

      // Occasional small offset pair to avoid perfect circular clumps.
      if (ridgeNoise > 0.58f || clusterNoise > 0.74f) {
        float secondaryAngle =
            (mRockNoise.noise(centerX * 0.17f + 910.0f,
                              centerZ * 0.17f - 510.0f) *
                 0.5f +
             0.5f) *
            TWO_PI;
        float secondaryDist = clusterRadius * (0.75f + ridgeNoise * 0.35f);
        float secondaryX = centerX + std::cos(secondaryAngle) * secondaryDist;
        float secondaryZ = centerZ + std::sin(secondaryAngle) * secondaryDist;
        trySpawnRock(secondaryX, secondaryZ, 0.42f + ridgeNoise * 0.20f, 0.36f);
        trySpawnRock(secondaryX + std::cos(secondaryAngle + 1.2f) * 0.9f,
                     secondaryZ + std::sin(secondaryAngle + 1.2f) * 0.9f,
                     0.24f, 0.40f);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════
// GRASS/BUSH CLUSTERS — Plains
// ═══════════════════════════════════════════════════════════════

void TerrainSystem::spawnPlainsGrass(int cx, int cz, ChunkData &chunk) {
  float ws = mSettings.chunkWorldSize;
  float ox = cx * ws;
  float oz = cz * ws;
  float spacing = 6.0f;
  int grid = (int)(ws / spacing);
  float biomeUV = 0.2f; // Plains = 1/5
  float grassDensity = std::clamp(mSettings.grassDensity, 0.0f, 1.0f);
  float grassScale = std::max(0.1f, mSettings.grassScale);

  for (int gz = 0; gz < grid; ++gz) {
    for (int gx = 0; gx < grid; ++gx) {
      float wx = ox + (gx + 0.5f) * spacing;
      float wz = oz + (gz + 0.5f) * spacing;

      if (getBiome(wx, wz) != BiomeType::Plains)
        continue;

      if (grassDensity <= 0.0001f)
        continue;

      float val =
          mDetailNoise.noise(wx * 0.4f + 500.0f, wz * 0.4f) * 0.5f + 0.5f;
      if (val > grassDensity)
        continue;

      float jx = mDetailNoise.noise(wx * 2.0f, wz * 2.5f) * spacing * 0.3f;
      float jz =
          mDetailNoise.noise(wx * 2.5f + 30.0f, wz * 2.0f) * spacing * 0.3f;
      wx += jx;
      wz += jz;

      float groundY = sampleHeight(wx, wz);
      if (groundY < mSettings.seaLevel)
        continue;

      std::string name = "grass_" + std::to_string(cx) + "_" +
                         std::to_string(cz) + "_" + std::to_string(gx) + "_" +
                         std::to_string(gz);
      std::vector<OBJModel::VertexData> verts;

      // Bush cluster — small sphere
      float sizeJitter =
          0.7f +
          (mDetailNoise.noise(wx * 1.1f + 120.0f, wz * 1.1f - 90.0f) * 0.5f +
           0.5f) *
              0.9f;
      float grassPatchScale = (0.55f + val * 0.90f) * grassScale * sizeJitter;
      float rotY =
          mDetailNoise.noise(wx * 0.9f + 77.0f, wz * 0.9f - 13.0f) * TWO_PI;
      addPrefabInstance("prefab_grass", {wx, groundY, wz},
                        glm::vec3(grassPatchScale),
                        glm::vec3(0.0f, rotY, 0.0f), chunk);

      // Occasional tall flower (narrow cone)
      if (val > 0.15f) {
        float fScale = (0.8f + val * 0.5f) * grassScale * sizeJitter;
        float flowerX = wx + (val - 0.5f) * 0.3f;
        float flowerZ = wz + (val * 2.0f - 1.0f) * 0.2f;
        float flowerRot =
            mDetailNoise.noise(flowerX * 2.0f, flowerZ * 2.0f + 31.0f) *
            TWO_PI;
        addPrefabInstance("prefab_flower", {flowerX, groundY, flowerZ},
                          glm::vec3(fScale), glm::vec3(0.0f, flowerRot, 0.0f),
                          chunk);
      }

      // Occasional low poly bush
      float bushVal =
          mDetailNoise.noise(wx * 0.2f + 900.0f, wz * 0.2f - 600.0f) * 0.5f +
          0.5f;
      if (bushVal < grassDensity * 0.6f) {
        float bushScale =
            (0.6f + bushVal * 0.8f) * grassScale * sizeJitter;
        float bushX = wx + (bushVal - 0.5f) * 0.8f;
        float bushZ = wz + (0.5f - bushVal) * 0.7f;
        addPrefabInstance("prefab_bush", {bushX, groundY, bushZ},
                          glm::vec3(bushScale), glm::vec3(0), chunk);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════
// VEGETATION DISPATCHER
// ═══════════════════════════════════════════════════════════════

void TerrainSystem::spawnVegetation(int cx, int cz, ChunkData &chunk) {
  if (!mSettings.spawnVegetation)
    return;

  BiomeType dominant = chunk.dominantBiome;

  if (mSettings.singleBiomeOnly) {
    spawnTreesForest(cx, cz, chunk);
    spawnPlainsGrass(cx, cz, chunk);
  } else {
    switch (dominant) {
    case BiomeType::Forest:
      spawnTreesForest(cx, cz, chunk);
      break;
    case BiomeType::Desert:
      spawnDesertCacti(cx, cz, chunk);
      break;
    case BiomeType::Tundra:
      spawnTundraDecor(cx, cz, chunk);
      break;
    case BiomeType::Plains:
      spawnPlainsGrass(cx, cz, chunk);
      break;
    default:
      break;
    }
  }

  // Rocks are biome-filtered inside spawnRocksMountain(); invoke every chunk so
  // mixed-biome chunks (eg forest-dominant with mountain pockets) still get
  // rocks.
  if (mSettings.spawnRocks) {
    spawnRocksMountain(cx, cz, chunk);
  }
}

// ═══════════════════════════════════════════════════════════════
// CHUNK LOAD / UNLOAD
// ═══════════════════════════════════════════════════════════════

void TerrainSystem::loadChunkAsync(int cx, int cz, int lod,
                                   bool withCollision, bool useGpuTerrain) {
  mInFlight[{cx, cz}] = true;

  // Capture noise objects by VALUE so the lambda is thread-safe.
  // PerlinNoise is small and cheap to copy.
  auto noise = mNoise;
  auto tempNoise = mTempNoise;
  auto moistNoise = mMoistNoise;
  auto treeNoise = mTreeNoise;
  auto rockNoise = mRockNoise;
  auto detailNoise = mDetailNoise;
  TerrainSettings settings = mSettings;
  const uint64_t generationId = mGenerationId;

  // Helper lambdas that capture only the copied noise objects
  auto sampleH = [&, noise, detailNoise, tempNoise, moistNoise,
                  settings](float wx, float wz) -> float {
    TerrainMacroSample sample =
        sampleTerrainMacro(settings, noise, tempNoise, moistNoise, wx, wz);
    return computeTerrainHeightValue(settings, sample, noise, detailNoise);
  };

  // Capture 'this' pointer only for mPendingReady / mPendingMutex push
  auto *self = this;
  auto pending = std::make_shared<PendingChunk>();
  pending->cx = cx;
  pending->cz = cz;
  pending->generationId = generationId;
  pending->terrainLod = lod;
  pending->useGpuTerrain = useGpuTerrain;

  auto fut = std::async(std::launch::async, [pending, cx, cz, settings, noise,
                                             tempNoise, moistNoise, treeNoise,
                                             rockNoise, detailNoise, sampleH,
                                             lod, withCollision, useGpuTerrain,
                                             generationId, self]() mutable {
    // ── 1. Terrain mesh (pure CPU) ──────────────────────────
    int renderSiz =
        useGpuTerrain
            ? std::max(4, settings.chunkSize)
            : std::max(4, settings.chunkSize >> std::clamp(lod, 0, 4));
    int physicsSiz = withCollision ? settings.chunkSize : 0;
    float ws = settings.chunkWorldSize;
    float renderStep = ws / (float)renderSiz;
    float physicsStep = withCollision ? (ws / (float)physicsSiz) : 0.0f;
    float ox = cx * ws;
    float oz = cz * ws;
    float uvS = 1.0f / (float)renderSiz;

    auto classifyBiome = [&](float wx, float wz) -> BiomeType {
      TerrainMacroSample sample =
          sampleTerrainMacro(settings, noise, tempNoise, moistNoise, wx, wz);
      return classifyLandscapeBiome(settings, sample);
    };

    // Determine dominant biome from center
    float midX = ox + ws * 0.5f;
    float midZ = oz + ws * 0.5f;
    BiomeType dom = classifyBiome(midX, midZ);
    pending->dominantBiome = dom;

    std::vector<std::vector<float>> hGrid(renderSiz + 1,
                                          std::vector<float>(renderSiz + 1));
    std::vector<std::vector<float>> bGrid(renderSiz + 1,
                                          std::vector<float>(renderSiz + 1));
    float minHeight = std::numeric_limits<float>::infinity();
    float maxHeight = -std::numeric_limits<float>::infinity();
    for (int z = 0; z <= renderSiz; ++z)
      for (int x = 0; x <= renderSiz; ++x) {
        float wx = ox + x * renderStep, wz = oz + z * renderStep;
        BiomeType biomeAtVertex = classifyBiome(wx, wz);
        hGrid[z][x] = sampleH(wx, wz);
        bGrid[z][x] = (float)(int)biomeAtVertex / 5.0f;
        minHeight = std::min(minHeight, hGrid[z][x]);
        maxHeight = std::max(maxHeight, hGrid[z][x]);
      }
    pending->minHeight = minHeight;
    pending->maxHeight = maxHeight;

    if (useGpuTerrain) {
      const uint32_t sc = (uint32_t)(renderSiz + 1);
      pending->gpuSampleCount = sc;
      pending->gpuHeightSamples.resize((size_t)sc * sc);
      pending->gpuBiomeSamples.resize((size_t)sc * sc);
      for (int z = 0; z <= renderSiz; ++z) {
        for (int x = 0; x <= renderSiz; ++x) {
          const size_t idx = (size_t)z * sc + x;
          pending->gpuHeightSamples[idx] = hGrid[z][x];
          pending->gpuBiomeSamples[idx] = bGrid[z][x];
        }
      }
    }

    auto getNorm = [&](int x, int z) -> glm::vec3 {
      float hL = (x > 0) ? hGrid[z][x - 1] : hGrid[z][x];
      float hR = (x < renderSiz) ? hGrid[z][x + 1] : hGrid[z][x];
      float hD = (z > 0) ? hGrid[z - 1][x] : hGrid[z][x];
      float hU = (z < renderSiz) ? hGrid[z + 1][x] : hGrid[z][x];
      return glm::normalize(
          glm::vec3(-(hR - hL), 2.0f * renderStep, -(hU - hD)));
    };

    if (!useGpuTerrain) {
      pending->terrainVerts.reserve(renderSiz * renderSiz * 6);
      for (int z = 0; z < renderSiz; ++z)
        for (int x = 0; x < renderSiz; ++x) {
          float wx0 = ox + x * renderStep, wx1 = ox + (x + 1) * renderStep;
          float wz0 = oz + z * renderStep, wz1 = oz + (z + 1) * renderStep;
          glm::vec3 p00(wx0, hGrid[z][x], wz0);
          glm::vec3 p10(wx1, hGrid[z][x + 1], wz0);
          glm::vec3 p01(wx0, hGrid[z + 1][x], wz1);
          glm::vec3 p11(wx1, hGrid[z + 1][x + 1], wz1);
          glm::vec2 uv00(x * uvS, bGrid[z][x]),
              uv10((x + 1) * uvS, bGrid[z][x + 1]);
          glm::vec2 uv01(x * uvS, bGrid[z + 1][x]),
              uv11((x + 1) * uvS, bGrid[z + 1][x + 1]);
          auto n00 = getNorm(x, z), n10 = getNorm(x + 1, z),
               n01 = getNorm(x, z + 1), n11 = getNorm(x + 1, z + 1);
          pending->terrainVerts.push_back({p00, uv00, n00});
          pending->terrainVerts.push_back({p01, uv01, n01});
          pending->terrainVerts.push_back({p10, uv10, n10});
          pending->terrainVerts.push_back({p10, uv10, n10});
          pending->terrainVerts.push_back({p01, uv01, n01});
          pending->terrainVerts.push_back({p11, uv11, n11});
        }
    }

    // Flatten hGrid into a row-major float array for HeightFieldShape.
    // HeightFieldShape expects samples[z * sampleCount + x].
    if (withCollision) {
      const uint32_t sc = (uint32_t)(physicsSiz + 1);
      pending->heightSamples.resize((size_t)sc * sc);
      pending->heightSampleCount = sc;
      for (int z = 0; z <= physicsSiz; ++z)
        for (int x = 0; x <= physicsSiz; ++x) {
          float wx = ox + x * physicsStep;
          float wz = oz + z * physicsStep;
          pending->heightSamples[(size_t)z * sc + x] = sampleH(wx, wz);
        }
    }

    // ── 2. Water plane (pure CPU) ────────────────────────────
    if (settings.spawnWater && dom == BiomeType::Ocean) {
      pending->hasWater = true;
      constexpr int WRES = 4;
      float wstep = ws / (float)WRES;
      float y = settings.seaLevel;
      glm::vec3 n(0, 1, 0);
      glm::vec2 uv(0, 0);
      pending->waterVerts.reserve(WRES * WRES * 6);
      for (int z = 0; z < WRES; ++z)
        for (int x = 0; x < WRES; ++x) {
          float x0 = ox + x * wstep, x1 = ox + (x + 1) * wstep;
          float z0 = oz + z * wstep, z1 = oz + (z + 1) * wstep;
          glm::vec3 p00(x0, y, z0), p10(x1, y, z0), p01(x0, y, z1),
              p11(x1, y, z1);
          pending->waterVerts.push_back({p00, uv, n});
          pending->waterVerts.push_back({p01, uv, n});
          pending->waterVerts.push_back({p10, uv, n});
          pending->waterVerts.push_back({p10, uv, n});
          pending->waterVerts.push_back({p01, uv, n});
          pending->waterVerts.push_back({p11, uv, n});
        }
    }

    // ── 3. Vegetation placement (math only, no ECS) ──────────
    if (settings.spawnVegetation && dom == BiomeType::Forest) {
      float spacing = 10.0f / std::max(0.05f, settings.treeDensity);
      int grid = std::max(1, (int)(ws / spacing));
      for (int gz = 0; gz < grid; ++gz)
        for (int gx = 0; gx < grid; ++gx) {
          float wx = ox + (gx + 0.5f) * spacing;
          float wz = oz + (gz + 0.5f) * spacing;
          float tv = treeNoise.noise(wx * 0.1f + 5.0f, wz * 0.1f) * 0.5f + 0.5f;
          if (tv > settings.treeDensity)
            continue;
          float jx = treeNoise.noise(wx * 1.3f, wz * 1.7f) * spacing * 0.35f;
          float jz =
              treeNoise.noise(wx * 2.1f + 50.0f, wz * 1.9f) * spacing * 0.35f;
          wx += jx;
          wz += jz;
          float groundY = sampleH(wx, wz);
          if (groundY < settings.seaLevel)
            continue;
          TreeType type = chooseForestTreeType(treeNoise, wx, wz);
          const char *prefabName = treePrefabName(type);
          auto &mats = pending->instanceMatrices[prefabName];
          glm::vec3 scale(0.95f + tv * 0.85f);
          if (type == TreeType::Oak)
            scale = glm::vec3(1.05f + tv * 0.65f);
          else if (type == TreeType::Birch)
            scale = glm::vec3(0.85f + tv * 0.55f, 1.05f + tv * 0.80f,
                              0.85f + tv * 0.55f);
          float rotY = treeNoise.noise(wx * 1.1f, wz * 1.1f) * TWO_PI;
          float tiltMask =
              treeNoise.noise(wx * 0.9f + 11.0f, wz * 0.9f - 22.0f) * 0.5f +
              0.5f;
          float tiltAmt = 0.0f;
          if (tiltMask > 0.78f) {
            tiltAmt = glm::radians(2.0f + (tiltMask - 0.78f) * (8.0f / 0.22f));
          }
          float tiltX = treeNoise.noise(wx * 5.3f + 100.0f, wz * 4.7f) *
                        tiltAmt;
          float tiltZ = treeNoise.noise(wx * 4.1f + 200.0f, wz * 5.9f) *
                        tiltAmt;
          glm::mat4 m(1.0f);
          m = glm::translate(m, glm::vec3(wx, groundY, wz));
          m = glm::rotate(m, rotY, glm::vec3(0, 1, 0));
          m = glm::rotate(m, tiltX, glm::vec3(1, 0, 0));
          m = glm::rotate(m, tiltZ, glm::vec3(0, 0, 1));
          m = glm::scale(m, scale);
          mats.push_back(m);
        }
    }

    // Push to ready queue
    pending->generationId = generationId;
    std::lock_guard<std::mutex> lk(self->mPendingMutex);
    self->mPendingReady.push_back(std::move(*pending));
  });

  // Store future to keep it alive
  // Prune completed futures to avoid unbounded growth
  mChunkFutures.erase(std::remove_if(mChunkFutures.begin(), mChunkFutures.end(),
                                     [](std::future<void> &f) {
                                       return f.valid() &&
                                              f.wait_for(
                                                  std::chrono::seconds(0)) ==
                                                  std::future_status::ready;
                                     }),
                      mChunkFutures.end());
  mChunkFutures.push_back(std::move(fut));
}

void TerrainSystem::flushPendingChunks() {
  // Swap out the ready queue under the lock so writers don't block long
  std::vector<PendingChunk> ready;
  {
    std::lock_guard<std::mutex> lk(mPendingMutex);
    ready.swap(mPendingReady);
  }

  const int cameraChunkX =
      (mLastCameraChunk.x == INT_MAX) ? 0 : mLastCameraChunk.x;
  const int cameraChunkZ =
      (mLastCameraChunk.z == INT_MAX) ? 0 : mLastCameraChunk.z;
  const int uploadBudget =
      std::max(1, mSettings.maxCompletedChunksPerFrame > 0
                      ? mSettings.maxCompletedChunksPerFrame
                      : mSettings.maxChunkUploadsPerFrame);
  const size_t gpuUploadBudget =
      (mFrameGpuUploadBudgetBytes > 0)
          ? mFrameGpuUploadBudgetBytes
          : (size_t)std::max(1024 * 1024, mSettings.maxGpuUploadBytesPerFrame);
  size_t gpuBytesUploaded = mFrameGpuUploadBytesUsed;
  int uploadedThisFrame = 0;
  std::vector<PendingChunk> deferred;

  for (auto &pc : ready) {
    if (pc.generationId != mGenerationId) {
      mInFlight.erase({pc.cx, pc.cz});
      continue;
    }
    const size_t pendingGpuBytes =
        pc.gpuHeightSamples.size() * sizeof(float) +
        pc.gpuBiomeSamples.size() * sizeof(float);
    if (uploadedThisFrame >= uploadBudget) {
      deferred.push_back(std::move(pc));
      continue;
    }
    if (pc.useGpuTerrain && pendingGpuBytes > 0 &&
        gpuBytesUploaded + pendingGpuBytes > gpuUploadBudget &&
        uploadedThisFrame > 0) {
      deferred.push_back(std::move(pc));
      continue;
    }

    // Clear in-flight marker
    mInFlight.erase({pc.cx, pc.cz});
    // Skip if already loaded (e.g. double-queued during regenerate)
    if (mChunks.count({pc.cx, pc.cz}))
      continue;

    auto &reg = mScene->registry();
    const float ws = mSettings.chunkWorldSize;

    ChunkData cd;
    cd.terrainLod = pc.terrainLod;
    cd.dominantBiome = pc.dominantBiome;

    bool uploadedGpuTerrain = false;
    if (pc.useGpuTerrain && ensureGpuRenderer() && !pc.gpuHeightSamples.empty()) {
      TerrainGpuChunkUpload upload;
      upload.cx = pc.cx;
      upload.cz = pc.cz;
      upload.lod = pc.terrainLod;
      upload.worldSize = mSettings.chunkWorldSize;
      upload.sampleCount = pc.gpuSampleCount;
      upload.heights = std::move(pc.gpuHeightSamples);
      upload.biomes = std::move(pc.gpuBiomeSamples);
      upload.minHeight = pc.minHeight;
      upload.maxHeight = pc.maxHeight;
      uploadedGpuTerrain = mGpuRenderer->uploadChunk(upload);
      if (uploadedGpuTerrain) {
        gpuBytesUploaded += pendingGpuBytes;
        mFrameGpuUploadBytesUsed = gpuBytesUploaded;
      } else {
        mGpuTerrainFallbackActive = true;
        mStats.gpuTerrainFallback = true;
      }
    }

    if (!uploadedGpuTerrain) {
      std::string name =
          "terrain_" + std::to_string(pc.cx) + "_" + std::to_string(pc.cz);

    // GPU upload — terrain mesh
      if (pc.terrainVerts.empty())
        pc.terrainVerts = generateChunkMeshLod(pc.cx, pc.cz, pc.terrainLod);
      auto model = std::make_unique<OBJModel>();
      model->loadFromVertices(pc.terrainVerts, name);
      OBJHandle terrainHandle = {};
      if (mAssets)
        terrainHandle = mAssets->registerRuntimeOBJ(name, std::move(model));
      OBJModel *terrainModel =
          mAssets ? mAssets->getOBJ(terrainHandle) : nullptr;
      if (!terrainHandle.valid() || !terrainModel)
        continue;

      EntityId eid = mScene->createEmptyEntity(name);
      reg.emplace<TransientComponent>(eid);
      auto &t = reg.get<TransformComponent>(eid);
      t.position = glm::vec3(0.0f);
      t.scale = glm::vec3(1.0f);
      auto &bounds = reg.emplace<BoundsComponent>(eid);
      bounds.centerOffset =
          glm::vec3(pc.cx * ws + ws * 0.5f, 0.0f, pc.cz * ws + ws * 0.5f);
      const float verticalRadius =
          std::max(24.0f, mSettings.heightScale * 4.0f);
      bounds.radius =
          std::sqrt(ws * ws * 0.5f + verticalRadius * verticalRadius);
      reg.emplace<MeshComponent>(eid);
      auto &mesh = reg.get<MeshComponent>(eid);
      mesh.objModel = terrainModel;
      mesh.objHandle = terrainHandle;
      mesh.type = MeshComponent::AssetType::OBJ;
      mesh.visible = true;
      mesh.castsShadow = true;
      mesh.isTerrain = true;
      mesh.assetId = name;

      cd.entity = eid;
      cd.terrainAssetId = name;
    }

    // GPU upload — water plane
    if (pc.hasWater && !pc.waterVerts.empty()) {
      std::string wname =
          "water_" + std::to_string(pc.cx) + "_" + std::to_string(pc.cz);
      auto waterMdl = std::make_unique<OBJModel>();
      waterMdl->loadFromVertices(pc.waterVerts, wname);
      OBJHandle waterHandle = {};
      if (mAssets)
        waterHandle = mAssets->registerRuntimeOBJ(wname, std::move(waterMdl));
      OBJModel *waterModel = mAssets ? mAssets->getOBJ(waterHandle) : nullptr;
      if (waterHandle.valid() && waterModel) {
        EntityId weid = mScene->createEmptyEntity(wname);
        reg.emplace<TransientComponent>(weid);
        auto &wt = reg.get<TransformComponent>(weid);
        wt.position = glm::vec3(0.0f);
        wt.scale = glm::vec3(1.0f);
        auto &wbounds = reg.emplace<BoundsComponent>(weid);
        wbounds.centerOffset =
            glm::vec3(pc.cx * ws + ws * 0.5f, mSettings.seaLevel,
                      pc.cz * ws + ws * 0.5f);
        wbounds.radius = ws * 0.72f;
        reg.emplace<MeshComponent>(weid);
        auto &wmesh = reg.get<MeshComponent>(weid);
        wmesh.objModel = waterModel;
        wmesh.objHandle = waterHandle;
        wmesh.type = MeshComponent::AssetType::OBJ;
        wmesh.visible = true;
        wmesh.castsShadow = false;
        wmesh.isTerrain = true;
        wmesh.isWater = true;
        wmesh.assetId = wname;
        cd.waterEntity = weid;
        cd.waterAssetId = wname;
        mStats.totalWaterPlanes++;
      }
    }

    // Apply pre-computed instance matrices to prefabs
    int interactiveTreeCount = 0;
    for (auto &[prefabName, matrices] : pc.instanceMatrices) {
      auto itP = mPrefabs.find(prefabName);
      if (itP == mPrefabs.end())
        continue;
      if (!mScene->registry().has<InstancedMeshComponent>(itP->second.entity))
        continue;
      auto &inst =
          mScene->registry().get<InstancedMeshComponent>(itP->second.entity);
      const PrefabData &pd = itP->second;
      for (auto &rawMat : matrices) {
        // Apply autoScale and baseRot from PrefabData
        glm::mat4 m = rawMat;
        // rawMat already has scale from the worker; fold in autoScale
        m = glm::rotate(m, pd.baseRot.x, glm::vec3(1, 0, 0));
        m = glm::rotate(m, pd.baseRot.y, glm::vec3(0, 1, 0));
        m = glm::rotate(m, pd.baseRot.z, glm::vec3(0, 0, 1));
        // Scale by autoScale component
        m = glm::scale(m, glm::vec3(pd.autoScale));
        inst.instanceTransforms.push_back(m);
        const size_t idx = inst.instanceTransforms.size() - 1;
        cd.prefabInstanceCounts[prefabName]++;
        cd.prefabInstanceMatrices[prefabName].push_back(m);
        cd.prefabInstanceGlobalIndices[prefabName].push_back(
            static_cast<uint32_t>(idx));
        cd.prefabInstanceEntities[prefabName].push_back(0);
        if (isInteractiveTreePrefab(prefabName)) {
          glm::vec3 pos = glm::vec3(m[3]);
          if (shouldSpawnInteractiveTree(mSettings, cameraChunkX, cameraChunkZ,
                                         pc.cx, pc.cz, pos, mTreeNoise,
                                         interactiveTreeCount)) {
            const size_t chunkSlot =
                cd.prefabInstanceMatrices[prefabName].empty()
                    ? 0
                    : cd.prefabInstanceMatrices[prefabName].size() - 1;
            glm::vec3 scale(glm::length(glm::vec3(m[0])),
                            glm::length(glm::vec3(m[1])),
                            glm::length(glm::vec3(m[2])));
            registerTreeInstance(prefabName, idx, chunkSlot, pos, scale, pc.cx,
                                 pc.cz, &cd);
            cd.treeCount++;
            mStats.totalTreeEntities++;
            interactiveTreeCount++;
          }
        }
      }
      inst.isDirty = true;
    }

    // Fall back: spawn remaining vegetation types synchronously (only for
    // non-Forest biomes which weren't handled in the async path)
    if (pc.instanceMatrices.empty())
      spawnVegetation(pc.cx, pc.cz, cd);
    else if (mSettings.spawnRocks)
      // Forest async path already injects trees via instanceMatrices; still add
      // biome-filtered rocks for this chunk.
      spawnRocksMountain(pc.cx, pc.cz, cd);

    // Apply painted instances for this chunk (persist across regeneration).
    auto itPaint = mPaintedInstances.find({pc.cx, pc.cz});
    if (itPaint != mPaintedInstances.end()) {
      int paintedInteractiveTreeCount = 0;
      for (auto &[prefabName, matrices] : itPaint->second.prefabMatrices) {
        auto itP = mPrefabs.find(prefabName);
        if (itP == mPrefabs.end())
          continue;
        if (!mScene->registry().has<InstancedMeshComponent>(itP->second.entity))
          continue;
        auto &inst = mScene->registry().get<InstancedMeshComponent>(
            itP->second.entity);

        for (const auto &m : matrices) {
          inst.instanceTransforms.push_back(m);
          const size_t idx = inst.instanceTransforms.size() - 1;
          cd.prefabInstanceCounts[prefabName]++;
          cd.prefabInstanceMatrices[prefabName].push_back(m);
          cd.prefabInstanceGlobalIndices[prefabName].push_back(
              static_cast<uint32_t>(idx));
          cd.prefabInstanceEntities[prefabName].push_back(0);
          if (isInteractiveTreePrefab(prefabName)) {
            glm::vec3 pos = glm::vec3(m[3]);
            if (shouldSpawnInteractiveTree(
                    mSettings, cameraChunkX, cameraChunkZ, pc.cx, pc.cz, pos,
                    mTreeNoise, paintedInteractiveTreeCount)) {
              const size_t chunkSlot =
                  cd.prefabInstanceMatrices[prefabName].empty()
                      ? 0
                      : cd.prefabInstanceMatrices[prefabName].size() - 1;
              glm::vec3 scale(glm::length(glm::vec3(m[0])),
                              glm::length(glm::vec3(m[1])),
                              glm::length(glm::vec3(m[2])));
              registerTreeInstance(prefabName, idx, chunkSlot, pos, scale,
                                   pc.cx, pc.cz, &cd);
              cd.treeCount++;
              mStats.totalTreeEntities++;
              paintedInteractiveTreeCount++;
            }
          } else if (prefabName == "prefab_rock") {
            cd.rockCount++;
            mStats.totalRockEntities++;
          }
        }
        inst.isDirty = true;
      }
    }

    // Register terrain collision with Jolt (HeightFieldShape)
    if (mPhysicsSystem && !pc.heightSamples.empty()) {
      cd.physicsBodyId = mPhysicsSystem->addTerrainChunk(
          pc.heightSamples, pc.heightSampleCount,
          glm::vec2(pc.cx * mSettings.chunkWorldSize,
                    pc.cz * mSettings.chunkWorldSize),
          mSettings.chunkWorldSize);
    }

    mStats.loadedChunks++;
    mStats.biomeCounts[(int)cd.dominantBiome]++;
    mChunks[{pc.cx, pc.cz}] = std::move(cd);
    uploadedThisFrame++;
  }

  if (!deferred.empty()) {
    std::lock_guard<std::mutex> lk(mPendingMutex);
    deferred.insert(deferred.end(),
                    std::make_move_iterator(mPendingReady.begin()),
                    std::make_move_iterator(mPendingReady.end()));
    mPendingReady = std::move(deferred);
  }
  syncGpuStats();
}

void TerrainSystem::unloadChunk(
    int cx, int cz,
    std::unordered_set<std::string> *deferredPrefabRebuilds) {
  auto it = mChunks.find({cx, cz});
  if (it == mChunks.end())
    return;

  ChunkData cd = std::move(it->second);
  if (mGpuRenderer)
    mGpuRenderer->removeChunk(cx, cz);

  // Remove tree entities belonging to this chunk
  for (auto &[prefabName, entities] : cd.prefabInstanceEntities) {
    for (auto eid : entities) {
      if (eid == 0)
        continue;
      if (mPhysicsSystem && mScene->registry().has<RigidbodyComponent>(eid)) {
        auto &rb = mScene->registry().get<RigidbodyComponent>(eid);
        mPhysicsSystem->removeBody(rb.bodyID);
      }
      mScene->deleteEntity(eid);
    }
  }

  // Remove terrain collision body from Jolt
  if (mPhysicsSystem && cd.physicsBodyId != 0xFFFFFFFF) {
    mPhysicsSystem->removeTerrainChunk(cd.physicsBodyId);
    cd.physicsBodyId = 0xFFFFFFFF;
  }

  // Remove instances from prefabs by rebuilding from remaining chunks.
  std::vector<std::string> affectedPrefabs;
  for (const auto &[prefabName, _] : cd.prefabInstanceMatrices) {
    affectedPrefabs.push_back(prefabName);
  }

  // Remove the chunk entry before rebuild so it won't be included.
  mChunks.erase(it);

  if (deferredPrefabRebuilds) {
    deferredPrefabRebuilds->insert(affectedPrefabs.begin(), affectedPrefabs.end());
  } else {
    for (const auto &prefabName : affectedPrefabs)
      rebuildPrefabInstances(prefabName);
  }

  // Remove water entity
  if (cd.waterEntity != 0 && mScene) {
    mScene->deleteEntity(cd.waterEntity);
    mStats.totalWaterPlanes--;
  }
  if (!cd.waterAssetId.empty() && mAssets)
    mAssets->releaseOBJ(cd.waterAssetId);

  // Remove terrain entity
  if (cd.entity != 0 && mScene)
    mScene->deleteEntity(cd.entity);
  if (!cd.terrainAssetId.empty() && mAssets)
    mAssets->releaseOBJ(cd.terrainAssetId);

  // Update stats
  mStats.loadedChunks--;
  mStats.biomeCounts[(int)cd.dominantBiome]--;
  mStats.totalTreeEntities -= cd.treeCount;
  mStats.totalRockEntities -= cd.rockCount;
  syncGpuStats();
}

void TerrainSystem::rebuildPrefabInstances(const std::string &prefabName) {
  if (!mScene)
    return;
  auto itPrefab = mPrefabs.find(prefabName);
  if (itPrefab == mPrefabs.end())
    return;
  if (!mScene->registry().has<InstancedMeshComponent>(
          itPrefab->second.entity))
    return;

  auto &reg = mScene->registry();
  auto &inst =
      reg.get<InstancedMeshComponent>(itPrefab->second.entity);
  inst.instanceTransforms.clear();

  auto &entityMap = mPrefabInstanceEntities[prefabName];
  entityMap.clear();

  size_t newIndex = 0;
  for (auto &[coord, cdata] : mChunks) {
    auto itM = cdata.prefabInstanceMatrices.find(prefabName);
    if (itM == cdata.prefabInstanceMatrices.end())
      continue;
    auto &mats = itM->second;
    auto &ents = cdata.prefabInstanceEntities[prefabName];
    auto &globals = cdata.prefabInstanceGlobalIndices[prefabName];

    const size_t count = mats.size();
    if (ents.size() < count)
      ents.resize(count, 0);
    else if (ents.size() > count)
      ents.resize(count);

    globals.clear();
    globals.reserve(count);
    inst.instanceTransforms.insert(inst.instanceTransforms.end(), mats.begin(),
                                   mats.end());

    for (size_t i = 0; i < count; ++i) {
      const uint32_t globalIndex = static_cast<uint32_t>(newIndex++);
      globals.push_back(globalIndex);

      const auto eid = ents[i];
      if (eid != 0 && reg.has<TreeComponent>(eid)) {
        auto &tree = reg.get<TreeComponent>(eid);
        tree.instanceIndex = globalIndex;
        tree.chunkInstanceSlot = static_cast<uint32_t>(i);
        tree.chunkX = coord.x;
        tree.chunkZ = coord.z;
        entityMap.push_back(eid);
      } else {
        ents[i] = 0;
        entityMap.push_back(0);
      }
    }
  }

  inst.isDirty = true;
}

void TerrainSystem::updateCollisionForChunk(int cx, int cz, ChunkData &cd) {
  if (!mPhysicsSystem)
    return;

  if (cd.physicsBodyId != 0xFFFFFFFF) {
    mPhysicsSystem->removeTerrainChunk(cd.physicsBodyId);
    cd.physicsBodyId = 0xFFFFFFFF;
  }

  const int cameraChunkX =
      (mLastCameraChunk.x == INT_MAX) ? cx : mLastCameraChunk.x;
  const int cameraChunkZ =
      (mLastCameraChunk.z == INT_MAX) ? cz : mLastCameraChunk.z;
  if (!chunkNeedsCollision(mSettings, cameraChunkX, cameraChunkZ, cx, cz))
    return;

  const int size = std::max(4, mSettings.chunkSize);
  const float ws = mSettings.chunkWorldSize;
  const float step = ws / (float)size;
  const float ox = cx * ws;
  const float oz = cz * ws;
  const uint32_t sc = (uint32_t)(size + 1);
  std::vector<float> heightSamples((size_t)sc * sc);
  for (int z = 0; z <= size; ++z) {
    for (int x = 0; x <= size; ++x) {
      const float wx = ox + x * step;
      const float wz = oz + z * step;
      heightSamples[(size_t)z * sc + x] = sampleHeight(wx, wz);
    }
  }
  cd.physicsBodyId =
      mPhysicsSystem->addTerrainChunk(heightSamples, sc, glm::vec2(ox, oz), ws);
}

void TerrainSystem::rebuildChunkTerrain(int cx, int cz, bool rebuildPhysics) {
  auto it = mChunks.find({cx, cz});
  if (it == mChunks.end() || !mScene || !mAssets)
    return;

  ChunkData &cd = it->second;
  const std::string name =
      "terrain_" + std::to_string(cx) + "_" + std::to_string(cz);

  if (gpuTerrainActive()) {
    if (mGpuRenderer)
      mGpuRenderer->updateChunkLod(cx, cz, cd.terrainLod);

    if (rebuildPhysics && mGpuRenderer) {
      const int size = std::max(4, mSettings.chunkSize);
      const uint32_t sc = (uint32_t)(size + 1);
      const float ws = mSettings.chunkWorldSize;
      const float step = ws / (float)size;
      const float ox = cx * ws;
      const float oz = cz * ws;
      TerrainGpuChunkUpload upload;
      upload.cx = cx;
      upload.cz = cz;
      upload.lod = cd.terrainLod;
      upload.worldSize = ws;
      upload.sampleCount = sc;
      upload.heights.resize((size_t)sc * sc);
      upload.biomes.resize((size_t)sc * sc);
      upload.minHeight = std::numeric_limits<float>::infinity();
      upload.maxHeight = -std::numeric_limits<float>::infinity();
      for (int z = 0; z <= size; ++z) {
        for (int x = 0; x <= size; ++x) {
          const float wx = ox + x * step;
          const float wz = oz + z * step;
          const size_t idx = (size_t)z * sc + x;
          const float h = sampleHeight(wx, wz);
          upload.heights[idx] = h;
          upload.biomes[idx] = (float)(int)getBiome(wx, wz) / 5.0f;
          upload.minHeight = std::min(upload.minHeight, h);
          upload.maxHeight = std::max(upload.maxHeight, h);
        }
      }
      if (!mGpuRenderer->uploadChunk(upload)) {
        mGpuTerrainFallbackActive = true;
        mStats.gpuTerrainFallback = true;
      }
    }

    if (rebuildPhysics)
      updateCollisionForChunk(cx, cz, cd);
    syncGpuStats();
    return;
  }

  auto model = std::make_unique<OBJModel>();
  model->loadFromVertices(generateChunkMeshLod(cx, cz, cd.terrainLod), name);
  OBJHandle handle = mAssets->registerRuntimeOBJ(name, std::move(model));
  OBJModel *terrainModel = mAssets->getOBJ(handle);
  if (!handle.valid() || !terrainModel)
    return;

  auto &reg = mScene->registry();
  if (cd.entity != 0 && reg.has<MeshComponent>(cd.entity)) {
    auto &mesh = reg.get<MeshComponent>(cd.entity);
    mesh.objModel = terrainModel;
    mesh.objHandle = handle;
    mesh.type = MeshComponent::AssetType::OBJ;
    mesh.visible = true;
    mesh.castsShadow = true;
    mesh.isTerrain = true;
    if (!reg.has<BoundsComponent>(cd.entity))
      reg.emplace<BoundsComponent>(cd.entity);
    auto &bounds = reg.get<BoundsComponent>(cd.entity);
    const float ws = mSettings.chunkWorldSize;
    bounds.centerOffset =
        glm::vec3(cx * ws + ws * 0.5f, 0.0f, cz * ws + ws * 0.5f);
    const float verticalRadius = std::max(24.0f, mSettings.heightScale * 4.0f);
    bounds.radius =
        std::sqrt(ws * ws * 0.5f + verticalRadius * verticalRadius);
  }
  cd.terrainAssetId = name;

  if (rebuildPhysics)
    updateCollisionForChunk(cx, cz, cd);
}

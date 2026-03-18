#include "TerrainSystem.h"
#include "Assets/AssetManager.h"
#include "Assets/OBJModel.h"
#include "Assets/UFBXModel.h"
#include "ECS/Systems/PhysicsSystem.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
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

static void applyVegetationCullProfile(const std::string &prefabName,
                                       InstancedMeshComponent &inst) {
  inst.maxDrawDistance = 220.0f;
  inst.shadowMaxDrawDistance = 140.0f;
  inst.instanceCullRadius = 6.0f;

  if (prefabName == "prefab_pine" || prefabName == "prefab_oak" ||
      prefabName == "prefab_birch" || prefabName == "prefab_deadtree") {
    inst.maxDrawDistance = 320.0f;
    inst.shadowMaxDrawDistance = 200.0f;
    inst.instanceCullRadius = 9.0f;
  } else if (prefabName == "prefab_rock") {
    inst.maxDrawDistance = 180.0f;
    inst.shadowMaxDrawDistance = 110.0f;
    inst.instanceCullRadius = 4.0f;
  } else if (prefabName == "prefab_grass" || prefabName == "prefab_flower") {
    inst.maxDrawDistance = 90.0f;
    inst.shadowMaxDrawDistance = 45.0f;
    inst.instanceCullRadius = 1.4f;
  } else if (prefabName == "prefab_bush") {
    inst.maxDrawDistance = 140.0f;
    inst.shadowMaxDrawDistance = 85.0f;
    inst.instanceCullRadius = 2.8f;
  } else if (prefabName == "prefab_cactus") {
    inst.maxDrawDistance = 240.0f;
    inst.shadowMaxDrawDistance = 150.0f;
    inst.instanceCullRadius = 5.5f;
  }
}

// Generate a tapered cylinder (trunk shapes)
static void addCylinder(std::vector<OBJModel::VertexData> &verts,
                        glm::vec3 base, float radiusBot, float radiusTop,
                        float height, int segments, float biomeUV) {
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
    glm::vec2 uv(0.0f, biomeUV);

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
                    float radius, float height, int segments, float biomeUV) {
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
    glm::vec2 uv(0.0f, biomeUV);

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
    glm::vec2 uv(0.0f, biomeUV);
    verts.push_back({base, uv, nDown});
    verts.push_back({p1, uv, nDown});
    verts.push_back({p0, uv, nDown});
  }
}

// Generate a rough sphere (for oak canopy, rocks)
static void addSphere(std::vector<OBJModel::VertexData> &verts,
                      glm::vec3 center, float radius, int rings, int sectors,
                      float biomeUV, float roughness = 0.0f,
                      const PerlinNoise *noiseGen = nullptr) {
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
      glm::vec2 uv(0.0f, biomeUV);

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
                         float biomeUV) {
  glm::vec3 n(0, 1, 0);
  glm::vec2 uv(0.0f, biomeUV);
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
                   glm::vec3 maxC, float biomeUV) {
  glm::vec3 corners[8] = {{minC.x, minC.y, minC.z}, {maxC.x, minC.y, minC.z},
                          {maxC.x, maxC.y, minC.z}, {minC.x, maxC.y, minC.z},
                          {minC.x, minC.y, maxC.z}, {maxC.x, minC.y, maxC.z},
                          {maxC.x, maxC.y, maxC.z}, {minC.x, maxC.y, maxC.z}};
  glm::vec2 uv(0.0f, biomeUV);
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

// ═══════════════════════════════════════════════════════════════
// INITIALIZATION & LIFECYCLE
// ═══════════════════════════════════════════════════════════════

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
  mStats = {};

  initPrefabs();

  std::cout << "[Terrain] Initialized with seed " << mSettings.seed
            << " | biomes, vegetation, rocks, water" << std::endl;
}

void TerrainSystem::applySettings(const TerrainSettings &settings) {
  if (settings.chunkSize != mSettings.chunkSize ||
      std::abs(settings.chunkWorldSize - mSettings.chunkWorldSize) > 0.001f) {
    mHeightOffsets.clear();
    mPaintedInstances.clear();
  }
  mSettings = settings;
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
  std::cout << "[Terrain] Regenerated with seed " << mSettings.seed
            << std::endl;
}

void TerrainSystem::shutdown() {
  std::vector<ChunkCoord> all;
  for (auto &[coord, data] : mChunks)
    all.push_back(coord);
  for (auto &coord : all)
    unloadChunk(coord.x, coord.z);
  clearPrefabs();
  mStats = {};
}

// ═══════════════════════════════════════════════════════════════
// BIOME DETERMINATION
// ═══════════════════════════════════════════════════════════════

BiomeType TerrainSystem::getBiome(float worldX, float worldZ) const {
  if (mSettings.singleBiomeOnly)
    return BiomeType::Plains;

  float bs = mSettings.biomeScale;
  float temp = mTempNoise.fbm(worldX * bs, worldZ * bs, 4, 2.0f, 0.5f);
  float moist = mMoistNoise.fbm(worldX * bs, worldZ * bs, 4, 2.0f, 0.5f);
  temp = temp * 0.5f + 0.5f; // Normalize to [0,1]
  moist = moist * 0.5f + 0.5f;

  // Whittaker-style biome classification
  if (temp < 0.25f) {
    return (moist > 0.5f) ? BiomeType::Tundra : BiomeType::Mountains;
  }
  if (temp > 0.72f) {
    return (moist < 0.4f) ? BiomeType::Desert : BiomeType::Plains;
  }
  if (moist > 0.55f)
    return BiomeType::Forest;
  if (moist < 0.3f)
    return BiomeType::Desert;

  // Check for ocean in low-lying moderate areas
  float baseH = mNoise.fbm(worldX * mSettings.noiseFrequency,
                           worldZ * mSettings.noiseFrequency, 3, 2.0f, 0.5f);
  if (baseH < -0.4f && moist > 0.35f)
    return BiomeType::Ocean;

  return BiomeType::Plains;
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
  float freq = mSettings.noiseFrequency;
  return mNoise.fbm(worldX * freq, worldZ * freq, mSettings.octaves,
                    mSettings.lacunarity, mSettings.gain);
}

float TerrainSystem::sampleRidgeNoise(float worldX, float worldZ) const {
  float freq = mSettings.noiseFrequency;
  return mNoise.ridgeNoise(worldX * freq, worldZ * freq, mSettings.octaves,
                           mSettings.lacunarity, mSettings.gain);
}

float TerrainSystem::shapeBiomeHeight(BiomeType biome, float baseH,
                                      float ridgeH) const {
  float hs = mSettings.heightScale;
  switch (biome) {
  case BiomeType::Ocean:
    return std::min(baseH * 3.0f, mSettings.seaLevel);
  case BiomeType::Plains:
    return baseH * 0.3f * hs;
  case BiomeType::Forest:
    return (baseH * 0.5f + 0.1f) * hs;
  case BiomeType::Desert:
    return std::abs(baseH) * 0.35f * hs;
  case BiomeType::Mountains:
    return ridgeH * 1.8f * hs;
  case BiomeType::Tundra:
    return (baseH * 0.25f + 0.3f) * hs * 0.5f;
  default:
    return baseH * hs;
  }
}

float TerrainSystem::sampleHeight(float worldX, float worldZ) const {
  float baseH = sampleBaseNoise(worldX, worldZ);
  float ridgeH = sampleRidgeNoise(worldX, worldZ);

  BiomeType biome = getBiome(worldX, worldZ);
  float h = shapeBiomeHeight(biome, baseH, ridgeH);

  // Cheap biome edge smoothing via threshold distance
  float bs = mSettings.biomeScale;
  float temp =
      mTempNoise.fbm(worldX * bs, worldZ * bs, 4, 2.0f, 0.5f) * 0.5f + 0.5f;
  float moist =
      mMoistNoise.fbm(worldX * bs, worldZ * bs, 4, 2.0f, 0.5f) * 0.5f + 0.5f;

  float tDists[] = {std::abs(temp - 0.25f), std::abs(temp - 0.72f)};
  float mDists[] = {std::abs(moist - 0.3f), std::abs(moist - 0.4f),
                    std::abs(moist - 0.5f), std::abs(moist - 0.55f)};
  float minDist = 1.0f;
  for (float d : tDists)
    minDist = std::min(minDist, d);
  for (float d : mDists)
    minDist = std::min(minDist, d);

  float blendZone = 0.08f;
  if (minDist < blendZone) {
    float blend = minDist / blendZone;
    float neutralH = baseH * 0.35f * mSettings.heightScale;
    h = h * blend + neutralH * (1.0f - blend);
  }

  // Erosion detail noise layers
  float freq = mSettings.noiseFrequency;
  float erosion =
      mNoise.noise(worldX * freq * 4.0f, worldZ * freq * 4.0f) * 0.15f;
  float micro =
      mDetailNoise.noise(worldX * freq * 8.0f, worldZ * freq * 8.0f) * 0.06f;
  float fine =
      mDetailNoise.noise(worldX * freq * 16.0f, worldZ * freq * 16.0f) * 0.02f;
  h += erosion + micro + fine;

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

        std::vector<glm::mat4> newMats;
        std::vector<uint32_t> newEnts;
        newMats.reserve(mats.size());
        newEnts.reserve(ents.size());

        for (size_t i = 0; i < mats.size(); ++i) {
          glm::vec3 pos = glm::vec3(mats[i][3]);
          float dx = pos.x - center.x;
          float dz = pos.z - center.z;
          float dist = std::sqrt(dx * dx + dz * dz);
          bool remove = dist <= radius;
          if (remove) {
            if (i < ents.size()) {
              uint32_t eid = ents[i];
              if (eid != 0) {
                if (mPhysicsSystem && reg.has<RigidbodyComponent>(eid)) {
                  auto &rb = reg.get<RigidbodyComponent>(eid);
                  mPhysicsSystem->removeBody(rb.bodyID);
                }
                mScene->deleteEntity(eid);
              }
            }
            if (prefabName == "prefab_pine") {
              if (cd.treeCount > 0)
                cd.treeCount--;
              if (mStats.totalTreeEntities > 0)
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
            if (i < ents.size())
              newEnts.push_back(ents[i]);
          }
        }

        mats.swap(newMats);
        ents.swap(newEnts);
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
        registerTreeInstance(prefabName, idx, pos, scale, cx, cz, &cd);
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

  // Always upload finished chunk jobs, even if the camera stayed in the same
  // chunk this frame.
  flushPendingChunks();

  int cx = (int)std::floor(cameraPos.x / mSettings.chunkWorldSize);
  int cz = (int)std::floor(cameraPos.z / mSettings.chunkWorldSize);

  ChunkCoord currentChunk = {cx, cz};
  if (currentChunk == mLastCameraChunk)
    return;
  mLastCameraChunk = currentChunk;

  // Get desired chunks sorted by distance (nearest loaded first)
  auto desired = getChunksByDistance(cx, cz, mSettings.viewDistance);

  // Unload chunks outside view distance
  std::vector<ChunkCoord> toUnload;
  for (auto &[coord, data] : mChunks) {
    bool found = false;
    for (auto &d : desired)
      if (d == coord) {
        found = true;
        break;
      }
    if (!found)
      toUnload.push_back(coord);
  }
  for (auto &coord : toUnload)
    unloadChunk(coord.x, coord.z);

  // Load missing chunks async (nearest first due to sorted order)
  for (auto &coord : desired) {
    if (mChunks.find(coord) == mChunks.end() &&
        mInFlight.find(coord) == mInFlight.end())
      loadChunkAsync(coord.x, coord.z,
                     computeChunkLod(cx, cz, coord.x, coord.z));
  }

  // Existing loaded chunks can move between LOD bands as the camera crosses
  // chunk boundaries. Rebuild render meshes only; collision stays full-res.
  for (auto &[coord, data] : mChunks) {
    const int desiredLod = computeChunkLod(cx, cz, coord.x, coord.z);
    if (desiredLod == data.terrainLod)
      continue;
    data.terrainLod = desiredLod;
    rebuildChunkTerrain(coord.x, coord.z, false);
  }
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
  if (dist <= 2)
    return 1;
  return 2;
}

int TerrainSystem::lodResolution(int lod) const {
  const int base = std::max(4, mSettings.chunkSize);
  const int shift = std::clamp(lod, 0, 2);
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
  applyVegetationCullProfile(name, inst);

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
  inst.isDirty = true;

  chunk.prefabInstanceCounts[name]++;
  chunk.prefabInstanceMatrices[name].push_back(m);
  return inst.instanceTransforms.size() - 1;
}

void TerrainSystem::registerTreeInstance(const std::string &prefabName,
                                         size_t instanceIndex,
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
  tree.prefabName = prefabName;
  tree.chunkX = cx;
  tree.chunkZ = cz;

  auto &vec = mPrefabInstanceEntities[prefabName];
  if (vec.size() <= instanceIndex)
    vec.resize(instanceIndex + 1, 0);
  vec[instanceIndex] = eid;
  if (chunk) {
    chunk->prefabInstanceEntities[prefabName].push_back(eid);
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

  auto &inst = reg.get<InstancedMeshComponent>(itPrefab->second.entity);
  auto &map = mPrefabInstanceEntities[tree.prefabName];
  const size_t idx = tree.instanceIndex;
  const size_t last = inst.instanceTransforms.empty()
                          ? 0
                          : inst.instanceTransforms.size() - 1;

  if (idx < inst.instanceTransforms.size()) {
    inst.instanceTransforms[idx] = inst.instanceTransforms[last];
    inst.instanceTransforms.pop_back();
    inst.isDirty = true;
  }

  if (idx < map.size() && !map.empty()) {
    uint32_t swappedEntity = map[last];
    map[idx] = swappedEntity;
    map.pop_back();
    if (swappedEntity != 0 && reg.has<TreeComponent>(swappedEntity)) {
      reg.get<TreeComponent>(swappedEntity).instanceIndex =
          static_cast<uint32_t>(idx);
    }
  }

  auto itChunk = mChunks.find({tree.chunkX, tree.chunkZ});
  if (itChunk != mChunks.end()) {
    auto &cd = itChunk->second;
    auto itCount = cd.prefabInstanceCounts.find(tree.prefabName);
    if (itCount != cd.prefabInstanceCounts.end() && itCount->second > 0)
      itCount->second--;
    if (cd.treeCount > 0)
      cd.treeCount--;
    auto itEnts = cd.prefabInstanceEntities.find(tree.prefabName);
    auto itMats = cd.prefabInstanceMatrices.find(tree.prefabName);
    if (itEnts != cd.prefabInstanceEntities.end() &&
        itMats != cd.prefabInstanceMatrices.end()) {
      auto &ents = itEnts->second;
      auto &mats = itMats->second;
      for (size_t i = 0; i < ents.size(); ++i) {
        if (ents[i] == treeEntity) {
          const size_t lastIdx = ents.size() - 1;
          ents[i] = ents[lastIdx];
          ents.pop_back();
          if (i < mats.size() && lastIdx < mats.size()) {
            mats[i] = mats[lastIdx];
            mats.pop_back();
          } else if (!mats.empty()) {
            mats.pop_back();
          }
          break;
        }
      }
    }
  }
  if (mStats.totalTreeEntities > 0)
    mStats.totalTreeEntities--;

  if (mPhysicsSystem && reg.has<RigidbodyComponent>(treeEntity)) {
    auto &rb = reg.get<RigidbodyComponent>(treeEntity);
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
  auto &map = mPrefabInstanceEntities[tree.prefabName];
  const size_t idx = tree.instanceIndex;
  const size_t last = inst.instanceTransforms.empty()
                          ? 0
                          : inst.instanceTransforms.size() - 1;
  glm::mat4 instM(1.0f);
  if (idx < inst.instanceTransforms.size())
    instM = inst.instanceTransforms[idx];

  if (idx < inst.instanceTransforms.size()) {
    inst.instanceTransforms[idx] = inst.instanceTransforms[last];
    inst.instanceTransforms.pop_back();
    inst.isDirty = true;
  }

  if (idx < map.size() && !map.empty()) {
    uint32_t swappedEntity = map[last];
    map[idx] = swappedEntity;
    map.pop_back();
    if (swappedEntity != 0 && reg.has<TreeComponent>(swappedEntity)) {
      reg.get<TreeComponent>(swappedEntity).instanceIndex =
          static_cast<uint32_t>(idx);
    }
  }

  auto itChunk = mChunks.find({tree.chunkX, tree.chunkZ});
  if (itChunk != mChunks.end()) {
    auto &cd = itChunk->second;
    auto itCount = cd.prefabInstanceCounts.find(tree.prefabName);
    if (itCount != cd.prefabInstanceCounts.end() && itCount->second > 0)
      itCount->second--;
    if (cd.treeCount > 0)
      cd.treeCount--;
  }
  if (mStats.totalTreeEntities > 0)
    mStats.totalTreeEntities--;

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
  tree.instanceIndex = std::numeric_limits<uint32_t>::max();
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
      applyVegetationCullProfile(prefabName, inst);

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
    applyVegetationCullProfile(prefabName, inst);

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
    addCylinder(verts, {0, 0, 0}, 0.2f, 0.1f, 4.0f, CYLINDER_SEGMENTS, 0.4f);
    addCone(verts, {0, 2.5f, 0}, 1.5f, 1.5f, CONE_SEGMENTS, 0.4f);
    addCone(verts, {0, 3.5f, 0}, 1.2f, 1.4f, CONE_SEGMENTS, 0.4f);
    addCone(verts, {0, 4.5f, 0}, 0.9f, 1.3f, CONE_SEGMENTS, 0.4f);
    addPrefabFromVerts("prefab_pine", verts);
    verts.clear();
  }

  // Oak
  addCylinder(verts, {0, 0, 0}, 0.25f, 0.2f, 3.0f, CYLINDER_SEGMENTS, 0.4f);
  addSphere(verts, {0, 3.2f, 0}, 2.0f, SPHERE_RINGS, SPHERE_SECTORS, 0.4f);
  addSphere(verts, {1.2f, 3.0f, 0}, 1.2f, SPHERE_RINGS, SPHERE_SECTORS, 0.4f);
  addSphere(verts, {-1.0f, 3.0f, 0.8f}, 1.1f, SPHERE_RINGS, SPHERE_SECTORS,
            0.4f);
  addPrefabFromVerts("prefab_oak", verts);
  verts.clear();

  // Birch
  addCylinder(verts, {0, 0, 0}, 0.1f, 0.08f, 4.5f, CYLINDER_SEGMENTS, 0.4f);
  addCone(verts, {0, 3.0f, 0}, 1.0f, 1.5f, CONE_SEGMENTS, 0.4f);
  addCone(verts, {0, 4.0f, 0}, 0.8f, 1.2f, CONE_SEGMENTS, 0.4f);
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
    addCylinder(verts, {0, 0, 0}, 0.15f, 0.1f, 3.0f, CYLINDER_SEGMENTS, 1.0f);
    addBox(verts, {0.0f, 1.5f, 0.1f}, {0.8f, 1.6f, 0.2f}, 1.0f);
    addBox(verts, {-0.6f, 2.2f, -0.1f}, {0.0f, 2.3f, 0.0f}, 1.0f);
    addPrefabFromVerts("prefab_deadtree", verts);
    verts.clear();
  }

  // Grass Cluster — only if no custom grass was loaded
  if (mPrefabs.find("prefab_grass") == mPrefabs.end()) {
    addSphere(verts, {0, 0, 0}, 0.4f, SPHERE_RINGS, SPHERE_SECTORS, 0.2f);
    addSphere(verts, {0.3f, 0, 0.3f}, 0.3f, SPHERE_RINGS, SPHERE_SECTORS, 0.2f);
    addSphere(verts, {-0.3f, 0, 0}, 0.35f, SPHERE_RINGS, SPHERE_SECTORS, 0.2f);
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
    addCylinder(verts, {0, 0, 0}, 0.02f, 0.02f, 0.8f, CYLINDER_SEGMENTS, 0.2f);
    addSphere(verts, {0, 0.8f, 0}, 0.15f, SPHERE_RINGS, SPHERE_SECTORS, 0.2f);
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

      // Determine tree type from noise
      // FORCE ALL TREES TO BE TALL PINES
      TreeType type = TreeType::Pine;

      std::string name = "tree_" + std::to_string(cx) + "_" +
                         std::to_string(cz) + "_" + std::to_string(gx) + "_" +
                         std::to_string(gz);
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

      if (type == TreeType::Pine) {
        size_t idx = addPrefabInstance("prefab_pine", {wx, groundY, wz},
                                       glm::vec3(1.0f + sizeVar), treeRot,
                                       chunk);
        if (idx != std::numeric_limits<size_t>::max()) {
          registerTreeInstance("prefab_pine", idx,
                               glm::vec3(wx, groundY, wz),
                               glm::vec3(1.0f + sizeVar), cx, cz, &chunk);
        }

      } else if (type == TreeType::Oak) {
        addPrefabInstance("prefab_oak", {wx, groundY, wz},
                          glm::vec3(1.0f + sizeVar), treeRot, chunk);
      } else {
        addPrefabInstance("prefab_birch", {wx, groundY, wz},
                          glm::vec3(1.0f + sizeVar), treeRot, chunk);
      }

      chunk.treeCount++;
      mStats.totalTreeEntities++;
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
        float scale = (0.5f + mDetailNoise.noise(wx * 5.0f, wz * 5.0f) * 0.8f) *
                      3.0f; // SCALE UP 3x
        addPrefabInstance("prefab_grass", {wx, groundY, wz}, glm::vec3(scale),
                          glm::vec3(0), chunk);
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

      chunk.treeCount++;
      mStats.totalTreeEntities++;
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

      chunk.treeCount++;
      mStats.totalTreeEntities++;
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
  float spacing = 8.0f;
  int grid = (int)(ws / spacing);
  float biomeUV = 0.8f; // Mountains = 4/5
  float rockDensity = std::clamp(mSettings.rockDensity, 0.0f, 1.0f);
  float rockScale = std::max(0.1f, mSettings.rockScale);

  for (int gz = 0; gz < grid; ++gz) {
    for (int gx = 0; gx < grid; ++gx) {
      float wx = ox + (gx + 0.5f) * spacing;
      float wz = oz + (gz + 0.5f) * spacing;

      BiomeType b = getBiome(wx, wz);
      // Allow rocks in "green" biomes too for richer terrain composition.
      if (b != BiomeType::Mountains && b != BiomeType::Tundra &&
          b != BiomeType::Forest && b != BiomeType::Plains)
        continue;

      if (rockDensity <= 0.0001f)
        continue;

      float val = mRockNoise.noise(wx * 0.25f, wz * 0.25f) * 0.5f + 0.5f;
      if (val > rockDensity)
        continue;

      float jx =
          mRockNoise.noise(wx * 1.3f + 100.0f, wz * 1.7f) * spacing * 0.35f;
      float jz =
          mRockNoise.noise(wx * 1.8f, wz * 1.2f + 100.0f) * spacing * 0.35f;
      wx += jx;
      wz += jz;

      float groundY = sampleHeight(wx, wz);
      if (groundY < mSettings.seaLevel)
        continue;

      float sizeJitter =
          0.7f +
          (mRockNoise.noise(wx * 0.9f + 77.0f, wz * 0.9f - 33.0f) * 0.5f +
           0.5f) *
              0.8f;
      float sr =
          (0.5f + mRockNoise.noise(wx * 3.0f, wz * 3.0f) * 1.5f) * rockScale *
          sizeJitter;
      sr = std::max(
          0.2f, sr); // Clamp scale so they don't corrupt matrices by inverting

      float rotY = mRockNoise.noise(wx * 0.6f, wz * 0.6f) * TWO_PI;
      addPrefabInstance("prefab_rock", {wx, groundY - sr * 0.3f, wz},
                        glm::vec3(sr * 1.2f, sr * 0.8f, sr * 1.1f),
                        glm::vec3(0, rotY, 0), chunk);

      chunk.rockCount++;
      mStats.totalRockEntities++;
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
      float bushR =
          (0.15f + val * 0.3f) * 3.0f * grassScale * sizeJitter; // SCALE UP 3x
      // addSphere(verts, {wx, groundY + bushR * 0.6f, wz}, bushR, 3, 4,
      // biomeUV);
      addPrefabInstance("prefab_grass", {wx, groundY, wz},
                        glm::vec3(bushR * 2.0f), glm::vec3(0), chunk);

      // Occasional tall flower (narrow cone)
      if (val > 0.15f) {
        float fScale = (0.8f + val * 0.5f) * grassScale * sizeJitter;
        float flowerX = wx + (val - 0.5f) * 0.3f;
        float flowerZ = wz + (val * 2.0f - 1.0f) * 0.2f;
        addPrefabInstance("prefab_flower", {flowerX, groundY, flowerZ},
                          glm::vec3(fScale), glm::vec3(0), chunk);
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

void TerrainSystem::loadChunkAsync(int cx, int cz, int lod) {
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

  // Helper lambdas that capture only the copied noise objects
  auto sampleH = [&, noise, detailNoise, tempNoise, moistNoise,
                  settings](float wx, float wz) -> float {
    // Inline simplified sampleHeight (calls captured noises)
    float freq = settings.noiseFrequency;
    float baseH = noise.fbm(wx * freq, wz * freq, settings.octaves,
                            settings.lacunarity, settings.gain);
    float ridgeH = noise.ridgeNoise(wx * freq, wz * freq, settings.octaves,
                                    settings.lacunarity, settings.gain);
    if (settings.singleBiomeOnly) {
      float h = baseH * 0.3f * settings.heightScale;
      h += noise.noise(wx * freq * 4.0f, wz * freq * 4.0f) * 0.15f +
           detailNoise.noise(wx * freq * 8.0f, wz * freq * 8.0f) * 0.06f +
           detailNoise.noise(wx * freq * 16.0f, wz * freq * 16.0f) * 0.02f;
      return h;
    }
    float bs = settings.biomeScale;
    float temp = tempNoise.fbm(wx * bs, wz * bs, 4, 2.0f, 0.5f) * 0.5f + 0.5f;
    float moist = moistNoise.fbm(wx * bs, wz * bs, 4, 2.0f, 0.5f) * 0.5f + 0.5f;

    BiomeType biome;
    if (temp < 0.25f)
      biome = (moist > 0.5f) ? BiomeType::Tundra : BiomeType::Mountains;
    else if (temp > 0.72f)
      biome = (moist < 0.4f) ? BiomeType::Desert : BiomeType::Plains;
    else if (moist > 0.55f)
      biome = BiomeType::Forest;
    else if (moist < 0.3f)
      biome = BiomeType::Desert;
    else
      biome = BiomeType::Plains;

    if (baseH < -0.4f && moist > 0.35f)
      biome = BiomeType::Ocean;

    float hs = settings.heightScale;
    float h = 0.0f;
    switch (biome) {
    case BiomeType::Ocean:
      h = std::min(baseH * 3.0f, settings.seaLevel);
      break;
    case BiomeType::Plains:
      h = baseH * 0.3f * hs;
      break;
    case BiomeType::Forest:
      h = (baseH * 0.5f + 0.1f) * hs;
      break;
    case BiomeType::Desert:
      h = std::abs(baseH) * 0.35f * hs;
      break;
    case BiomeType::Mountains:
      h = ridgeH * 1.8f * hs;
      break;
    case BiomeType::Tundra:
      h = (baseH * 0.25f + 0.3f) * hs * 0.5f;
      break;
    default:
      h = baseH * hs;
      break;
    }

    // Match sync path's biome-edge smoothing.
    float tDists[] = {std::abs(temp - 0.25f), std::abs(temp - 0.72f)};
    float mDists[] = {std::abs(moist - 0.3f), std::abs(moist - 0.4f),
                      std::abs(moist - 0.5f), std::abs(moist - 0.55f)};
    float minDist = 1.0f;
    for (float d : tDists)
      minDist = std::min(minDist, d);
    for (float d : mDists)
      minDist = std::min(minDist, d);

    const float blendZone = 0.08f;
    if (minDist < blendZone) {
      float blend = minDist / blendZone;
      float neutralH = baseH * 0.35f * hs;
      h = h * blend + neutralH * (1.0f - blend);
    }

    h += noise.noise(wx * freq * 4.0f, wz * freq * 4.0f) * 0.15f +
         detailNoise.noise(wx * freq * 8.0f, wz * freq * 8.0f) * 0.06f +
         detailNoise.noise(wx * freq * 16.0f, wz * freq * 16.0f) * 0.02f;
    return h;
  };

  // Capture 'this' pointer only for mPendingReady / mPendingMutex push
  auto *self = this;
  auto pending = std::make_shared<PendingChunk>();
  pending->cx = cx;
  pending->cz = cz;
  pending->terrainLod = lod;

  auto fut = std::async(std::launch::async, [pending, cx, cz, settings, noise,
                                             tempNoise, moistNoise, treeNoise,
                                             rockNoise, detailNoise, sampleH,
                                             lod,
                                             self]() mutable {
    // ── 1. Terrain mesh (pure CPU) ──────────────────────────
    int renderSiz = std::max(4, settings.chunkSize >> std::clamp(lod, 0, 2));
    int physicsSiz = settings.chunkSize;
    float ws = settings.chunkWorldSize;
    float renderStep = ws / (float)renderSiz;
    float physicsStep = ws / (float)physicsSiz;
    float ox = cx * ws;
    float oz = cz * ws;
    float uvS = 1.0f / (float)renderSiz;

    auto classifyBiome = [&](float wx, float wz) -> BiomeType {
      if (settings.singleBiomeOnly)
        return BiomeType::Plains;

      float freq = settings.noiseFrequency;
      float bs = settings.biomeScale;

      float temp = tempNoise.fbm(wx * bs, wz * bs, 4, 2.0f, 0.5f) * 0.5f + 0.5f;
      float moist =
          moistNoise.fbm(wx * bs, wz * bs, 4, 2.0f, 0.5f) * 0.5f + 0.5f;
      float baseH = noise.fbm(wx * freq, wz * freq, settings.octaves,
                              settings.lacunarity, settings.gain);

      BiomeType biome;
      if (temp < 0.25f)
        biome = (moist > 0.5f) ? BiomeType::Tundra : BiomeType::Mountains;
      else if (temp > 0.72f)
        biome = (moist < 0.4f) ? BiomeType::Desert : BiomeType::Plains;
      else if (moist > 0.55f)
        biome = BiomeType::Forest;
      else if (moist < 0.3f)
        biome = BiomeType::Desert;
      else
        biome = BiomeType::Plains;

      if (baseH < -0.4f && moist > 0.35f)
        biome = BiomeType::Ocean;
      return biome;
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
    for (int z = 0; z <= renderSiz; ++z)
      for (int x = 0; x <= renderSiz; ++x) {
        float wx = ox + x * renderStep, wz = oz + z * renderStep;
        BiomeType biomeAtVertex = classifyBiome(wx, wz);
        hGrid[z][x] = sampleH(wx, wz);
        bGrid[z][x] = (float)(int)biomeAtVertex / 5.0f;
      }

    auto getNorm = [&](int x, int z) -> glm::vec3 {
      float hL = (x > 0) ? hGrid[z][x - 1] : hGrid[z][x];
      float hR = (x < renderSiz) ? hGrid[z][x + 1] : hGrid[z][x];
      float hD = (z > 0) ? hGrid[z - 1][x] : hGrid[z][x];
      float hU = (z < renderSiz) ? hGrid[z + 1][x] : hGrid[z][x];
      return glm::normalize(
          glm::vec3(-(hR - hL), 2.0f * renderStep, -(hU - hD)));
    };

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

    // Flatten hGrid into a row-major float array for HeightFieldShape.
    // HeightFieldShape expects samples[z * sampleCount + x].
    const uint32_t sc = (uint32_t)(physicsSiz + 1);
    pending->heightSamples.resize((size_t)sc * sc);
    pending->heightSampleCount = sc;
    for (int z = 0; z <= physicsSiz; ++z)
      for (int x = 0; x <= physicsSiz; ++x) {
        float wx = ox + x * physicsStep;
        float wz = oz + z * physicsStep;
        pending->heightSamples[(size_t)z * sc + x] = sampleH(wx, wz);
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
      auto &mats = pending->instanceMatrices["prefab_pine"];
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
          float sv = 1.0f + tv;
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
          m = glm::scale(m, glm::vec3(sv));
          mats.push_back(m);
        }
    }

    // Push to ready queue
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

  for (auto &pc : ready) {
    // Clear in-flight marker
    mInFlight.erase({pc.cx, pc.cz});
    // Skip if already loaded (e.g. double-queued during regenerate)
    if (mChunks.count({pc.cx, pc.cz}))
      continue;

    std::string name =
        "terrain_" + std::to_string(pc.cx) + "_" + std::to_string(pc.cz);

    // GPU upload — terrain mesh
    auto model = std::make_unique<OBJModel>();
    model->loadFromVertices(pc.terrainVerts, name);
    OBJHandle terrainHandle = {};
    if (mAssets)
      terrainHandle = mAssets->registerRuntimeOBJ(name, std::move(model));
    OBJModel *terrainModel = mAssets ? mAssets->getOBJ(terrainHandle) : nullptr;
    if (!terrainHandle.valid() || !terrainModel)
      continue;

    EntityId eid = mScene->createEmptyEntity(name);
    auto &reg = mScene->registry();
    reg.emplace<TransientComponent>(eid);
    auto &t = reg.get<TransformComponent>(eid);
    t.position = glm::vec3(0.0f);
    t.scale = glm::vec3(1.0f);
    reg.emplace<MeshComponent>(eid);
    auto &mesh = reg.get<MeshComponent>(eid);
    mesh.objModel = terrainModel;
    mesh.objHandle = terrainHandle;
    mesh.type = MeshComponent::AssetType::OBJ;
    mesh.visible = true;
    mesh.castsShadow = true;
    mesh.isTerrain = true;
    mesh.assetId = name;

    ChunkData cd;
    cd.entity = eid;
    cd.terrainAssetId = name;
    cd.terrainLod = pc.terrainLod;
    cd.dominantBiome = pc.dominantBiome;

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
        cd.prefabInstanceCounts[prefabName]++;
        cd.prefabInstanceMatrices[prefabName].push_back(m);
        if (prefabName == "prefab_pine") {
          size_t idx = inst.instanceTransforms.size() - 1;
          glm::vec3 pos = glm::vec3(m[3]);
          glm::vec3 scale(glm::length(glm::vec3(m[0])),
                          glm::length(glm::vec3(m[1])),
                          glm::length(glm::vec3(m[2])));
          registerTreeInstance(prefabName, idx, pos, scale, pc.cx, pc.cz, &cd);
          cd.treeCount++;
          mStats.totalTreeEntities++;
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
          cd.prefabInstanceCounts[prefabName]++;
          cd.prefabInstanceMatrices[prefabName].push_back(m);
          if (prefabName == "prefab_pine") {
            size_t idx = inst.instanceTransforms.size() - 1;
            glm::vec3 pos = glm::vec3(m[3]);
            glm::vec3 scale(glm::length(glm::vec3(m[0])),
                            glm::length(glm::vec3(m[1])),
                            glm::length(glm::vec3(m[2])));
            registerTreeInstance(prefabName, idx, pos, scale, pc.cx, pc.cz,
                                 &cd);
            cd.treeCount++;
            mStats.totalTreeEntities++;
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
  }
}

void TerrainSystem::unloadChunk(int cx, int cz) {
  auto it = mChunks.find({cx, cz});
  if (it == mChunks.end())
    return;

  ChunkData cd = std::move(it->second);

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

  for (const auto &prefabName : affectedPrefabs) {
    auto itPrefab = mPrefabs.find(prefabName);
    if (itPrefab == mPrefabs.end() || !mScene)
      continue;
    if (!mScene->registry().has<InstancedMeshComponent>(
            itPrefab->second.entity))
      continue;
    auto &inst = mScene->registry().get<InstancedMeshComponent>(
        itPrefab->second.entity);
    inst.instanceTransforms.clear();

    std::vector<uint32_t> newEntityMap;
    size_t newIndex = 0;
    for (auto &[coord, cdata] : mChunks) {
      auto itM = cdata.prefabInstanceMatrices.find(prefabName);
      if (itM == cdata.prefabInstanceMatrices.end())
        continue;
      const auto &mats = itM->second;
      inst.instanceTransforms.insert(inst.instanceTransforms.end(), mats.begin(),
                                     mats.end());

      auto itE = cdata.prefabInstanceEntities.find(prefabName);
      if (itE != cdata.prefabInstanceEntities.end()) {
        const auto &ents = itE->second;
        const size_t count = mats.size();
        const size_t assignCount = std::min(count, ents.size());
        for (size_t i = 0; i < assignCount; ++i) {
          const auto eid = ents[i];
          if (eid != 0 && mScene->registry().has<TreeComponent>(eid)) {
            mScene->registry().get<TreeComponent>(eid).instanceIndex =
                static_cast<uint32_t>(newIndex);
          }
          newEntityMap.push_back(eid);
          newIndex++;
        }
        for (size_t i = assignCount; i < count; ++i) {
          newEntityMap.push_back(0);
          newIndex++;
        }
      } else {
        newIndex += mats.size();
      }
    }
    inst.isDirty = true;
    if (!newEntityMap.empty())
      mPrefabInstanceEntities[prefabName] = std::move(newEntityMap);
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

    const size_t count = mats.size();
    inst.instanceTransforms.insert(inst.instanceTransforms.end(), mats.begin(),
                                   mats.end());

    const size_t assignCount = std::min(count, ents.size());
    for (size_t i = 0; i < assignCount; ++i) {
      const auto eid = ents[i];
      if (eid != 0 && reg.has<TreeComponent>(eid)) {
        reg.get<TreeComponent>(eid).instanceIndex =
            static_cast<uint32_t>(newIndex);
      }
      entityMap.push_back(eid);
      newIndex++;
    }
    for (size_t i = assignCount; i < count; ++i) {
      entityMap.push_back(0);
      newIndex++;
    }
  }

  inst.isDirty = true;
}

void TerrainSystem::rebuildChunkTerrain(int cx, int cz, bool rebuildPhysics) {
  auto it = mChunks.find({cx, cz});
  if (it == mChunks.end() || !mScene || !mAssets)
    return;

  ChunkData &cd = it->second;
  const std::string name =
      "terrain_" + std::to_string(cx) + "_" + std::to_string(cz);

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
  }
  cd.terrainAssetId = name;

  if (mPhysicsSystem && rebuildPhysics) {
    if (cd.physicsBodyId != 0xFFFFFFFF) {
      mPhysicsSystem->removeTerrainChunk(cd.physicsBodyId);
      cd.physicsBodyId = 0xFFFFFFFF;
    }
    const int size = mSettings.chunkSize;
    const float ws = mSettings.chunkWorldSize;
    const float step = ws / (float)size;
    const float ox = cx * ws;
    const float oz = cz * ws;
    const uint32_t sc = (uint32_t)(size + 1);
    std::vector<float> heightSamples((size_t)sc * sc);
    for (int z = 0; z <= size; ++z) {
      for (int x = 0; x <= size; ++x) {
        float wx = ox + x * step;
        float wz = oz + z * step;
        heightSamples[(size_t)z * sc + x] = sampleHeight(wx, wz);
      }
    }
    cd.physicsBodyId = mPhysicsSystem->addTerrainChunk(
        heightSamples, sc, glm::vec2(ox, oz), ws);
  }
}

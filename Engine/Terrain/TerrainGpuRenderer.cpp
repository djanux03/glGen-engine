#include "TerrainGpuRenderer.h"

#include "Rendering/GLStateCache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {
struct GridVertex {
  glm::vec3 pos;
};

float wrapAngle(float a) {
  constexpr float twoPi = 6.28318530718f;
  while (a < 0.0f)
    a += twoPi;
  while (a >= twoPi)
    a -= twoPi;
  return a;
}

int wrapIndex(int idx, int count) {
  if (count <= 0)
    return 0;
  idx %= count;
  if (idx < 0)
    idx += count;
  return idx;
}

void mixHash(uint64_t &h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}
} // namespace

TerrainGpuRenderer::~TerrainGpuRenderer() { shutdown(); }

int64_t TerrainGpuRenderer::packKey(int cx, int cz) {
  const uint64_t ux = uint64_t(uint32_t(cx));
  const uint64_t uz = uint64_t(uint32_t(cz));
  return (int64_t)((ux << 32) | uz);
}

bool TerrainGpuRenderer::initialize(int chunkSize, int pageCapacity) {
  shutdown();

  GLint maxTextureSize = 0;
  GLint maxLayers = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);

  mChunkSize = std::max(4, chunkSize);
  mSampleEdge = mChunkSize + 1;
  if (mSampleEdge > maxTextureSize || maxLayers <= 0)
    return false;

  mPageCapacity =
      std::clamp(std::max(16, pageCapacity), 1, std::max(1, maxLayers));

  glGenTextures(1, &mHeightPages);
  glBindTexture(GL_TEXTURE_2D_ARRAY, mHeightPages);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R32F, mSampleEdge, mSampleEdge,
               mPageCapacity, 0, GL_RED, GL_FLOAT, nullptr);

  glGenTextures(1, &mBiomePages);
  glBindTexture(GL_TEXTURE_2D_ARRAY, mBiomePages);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R32F, mSampleEdge, mSampleEdge,
               mPageCapacity, 0, GL_RED, GL_FLOAT, nullptr);
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

  if (glGetError() != GL_NO_ERROR) {
    shutdown();
    return false;
  }

  for (int lod = 0; lod < kMaxLodBuckets; ++lod) {
    if (!buildGrid(lod)) {
      shutdown();
      return false;
    }
  }

  mFreeLayers.clear();
  mFreeLayers.reserve((size_t)mPageCapacity);
  for (int i = mPageCapacity - 1; i >= 0; --i)
    mFreeLayers.push_back(i);

  mActive = true;
  mStats = {};
  mStats.active = true;
  invalidatePassCaches();
  return true;
}

void TerrainGpuRenderer::shutdown() {
  for (int i = 0; i < kMaxLodBuckets; ++i) {
    if (mGridVao[i])
      glDeleteVertexArrays(1, &mGridVao[i]);
    if (mGridVbo[i])
      glDeleteBuffers(1, &mGridVbo[i]);
    if (mGridEbo[i])
      glDeleteBuffers(1, &mGridEbo[i]);
    if (mInstanceVbo[i])
      glDeleteBuffers(1, &mInstanceVbo[i]);
    mGridVao[i] = 0;
    mGridVbo[i] = 0;
    mGridEbo[i] = 0;
    mInstanceVbo[i] = 0;
    mIndexCount[i] = 0;
    mBuckets[i].clear();
    mUploadedBuckets[i].clear();
    mInstanceVboCapacity[i] = 0;
  }
  if (mHeightPages)
    glDeleteTextures(1, &mHeightPages);
  if (mBiomePages)
    glDeleteTextures(1, &mBiomePages);
  mHeightPages = 0;
  mBiomePages = 0;
  mChunks.clear();
  mFreeLayers.clear();
  mCandidates.clear();
  mHorizonScratch.clear();
  invalidatePassCaches();
  mActive = false;
  mStats = {};
}

void TerrainGpuRenderer::clear() {
  mChunks.clear();
  mFreeLayers.clear();
  mCandidates.clear();
  mHorizonScratch.clear();
  for (int i = mPageCapacity - 1; i >= 0; --i)
    mFreeLayers.push_back(i);
  for (auto &bucket : mBuckets)
    bucket.clear();
  for (auto &bucket : mUploadedBuckets)
    bucket.clear();
  mStats = {};
  mStats.active = mActive;
  ++mChunkRevision;
  invalidatePassCaches();
}

bool TerrainGpuRenderer::buildGrid(int lod) {
  const int resolution = std::max(4, mChunkSize >> std::clamp(lod, 0, 4));
  std::vector<GridVertex> verts;
  std::vector<uint32_t> indices;
  verts.reserve((size_t)(resolution + 1) * (resolution + 1) + 4 * resolution);
  indices.reserve((size_t)resolution * resolution * 6 + 4 * resolution * 6);

  auto addVertex = [&](float x, float z, float skirt) -> uint32_t {
    verts.push_back({glm::vec3(x, skirt, z)});
    return (uint32_t)verts.size() - 1;
  };

  for (int z = 0; z <= resolution; ++z) {
    for (int x = 0; x <= resolution; ++x) {
      addVertex((float)x / (float)resolution, (float)z / (float)resolution,
                0.0f);
    }
  }

  auto gridIndex = [&](int x, int z) -> uint32_t {
    return (uint32_t)(z * (resolution + 1) + x);
  };

  for (int z = 0; z < resolution; ++z) {
    for (int x = 0; x < resolution; ++x) {
      const uint32_t i00 = gridIndex(x, z);
      const uint32_t i10 = gridIndex(x + 1, z);
      const uint32_t i01 = gridIndex(x, z + 1);
      const uint32_t i11 = gridIndex(x + 1, z + 1);
      indices.insert(indices.end(), {i00, i01, i10, i10, i01, i11});
    }
  }

  auto addSkirtQuad = [&](uint32_t topA, uint32_t topB) {
    const glm::vec3 a = verts[topA].pos;
    const glm::vec3 b = verts[topB].pos;
    const uint32_t botA = addVertex(a.x, a.z, 1.0f);
    const uint32_t botB = addVertex(b.x, b.z, 1.0f);
    indices.insert(indices.end(), {topA, botA, topB, topB, botA, botB});
  };

  for (int x = 0; x < resolution; ++x)
    addSkirtQuad(gridIndex(x, 0), gridIndex(x + 1, 0));
  for (int x = 0; x < resolution; ++x)
    addSkirtQuad(gridIndex(x + 1, resolution), gridIndex(x, resolution));
  for (int z = 0; z < resolution; ++z)
    addSkirtQuad(gridIndex(0, z + 1), gridIndex(0, z));
  for (int z = 0; z < resolution; ++z)
    addSkirtQuad(gridIndex(resolution, z), gridIndex(resolution, z + 1));

  glGenVertexArrays(1, &mGridVao[lod]);
  glGenBuffers(1, &mGridVbo[lod]);
  glGenBuffers(1, &mGridEbo[lod]);
  glGenBuffers(1, &mInstanceVbo[lod]);

  // Attribute setup must bind the VAO directly. Other renderer paths still use
  // raw glBindVertexArray calls, so the cache can be stale during startup.
  glBindVertexArray(mGridVao[lod]);
  glBindBuffer(GL_ARRAY_BUFFER, mGridVbo[lod]);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(GridVertex)),
               verts.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GridVertex),
                        (void *)offsetof(GridVertex, pos));

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mGridEbo[lod]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               (GLsizeiptr)(indices.size() * sizeof(uint32_t)), indices.data(),
               GL_STATIC_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, mInstanceVbo[lod]);
  glEnableVertexAttribArray(7);
  glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(TerrainGpuInstance),
                        (void *)offsetof(TerrainGpuInstance, chunk0));
  glVertexAttribDivisor(7, 1);
  glEnableVertexAttribArray(8);
  glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, sizeof(TerrainGpuInstance),
                        (void *)offsetof(TerrainGpuInstance, chunk1));
  glVertexAttribDivisor(8, 1);

  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  mIndexCount[lod] = (GLsizei)indices.size();
  return glGetError() == GL_NO_ERROR;
}

void TerrainGpuRenderer::invalidatePassCaches() {
  for (auto &cache : mPassCache) {
    cache.valid = false;
    cache.renderKey = 0;
    cache.chunkRevision = 0;
    cache.stats = {};
    for (auto &bucket : cache.buckets)
      bucket.clear();
  }
}

uint64_t TerrainGpuRenderer::buildRenderKey(const glm::vec3 &cameraPos,
                                            bool shadowPass,
                                            bool enableHorizonCulling,
                                            int horizonSectors) const {
  uint64_t h = 1469598103934665603ull;
  auto quant = [](float v, float scale) -> int64_t {
    return (int64_t)std::llround(v * scale);
  };
  mixHash(h, (uint64_t)shadowPass);
  mixHash(h, (uint64_t)(enableHorizonCulling ? 1 : 0));
  mixHash(h, (uint64_t)std::clamp(horizonSectors, 0, 4096));
  mixHash(h, (uint64_t)quant(cameraPos.x, 4.0f));
  mixHash(h, (uint64_t)quant(cameraPos.y, 4.0f));
  mixHash(h, (uint64_t)quant(cameraPos.z, 4.0f));
  for (const glm::vec4 &p : mFrustumPlanes) {
    mixHash(h, (uint64_t)quant(p.x, 256.0f));
    mixHash(h, (uint64_t)quant(p.y, 256.0f));
    mixHash(h, (uint64_t)quant(p.z, 256.0f));
    mixHash(h, (uint64_t)quant(p.w, 8.0f));
  }
  return h;
}

bool TerrainGpuRenderer::bucketMatchesUploaded(
    int lod, const std::vector<TerrainGpuInstance> &bucket) const {
  const auto &uploaded = mUploadedBuckets[lod];
  if (bucket.size() != uploaded.size())
    return false;
  if (bucket.empty())
    return true;
  const size_t bytes = bucket.size() * sizeof(TerrainGpuInstance);
  return std::memcmp(bucket.data(), uploaded.data(), bytes) == 0;
}

void TerrainGpuRenderer::uploadBucketIfNeeded(
    int lod, const std::vector<TerrainGpuInstance> &bucket,
    TerrainGpuRendererStats &frameStats) {
  if (bucketMatchesUploaded(lod, bucket)) {
    ++frameStats.instanceUploadSkips;
    return;
  }

  const size_t bytes = bucket.size() * sizeof(TerrainGpuInstance);
  glBindBuffer(GL_ARRAY_BUFFER, mInstanceVbo[lod]);
  if (bytes > mInstanceVboCapacity[lod]) {
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_DRAW);
    mInstanceVboCapacity[lod] = bytes;
  }
  if (bytes > 0) {
    bool uploaded = false;
    if (mSubmissionBackend == RenderSubmissionBackend::Modern &&
        glMapBufferRange != nullptr) {
      void *mapped =
          glMapBufferRange(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes,
                           GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
      if (mapped) {
        std::memcpy(mapped, bucket.data(), bytes);
        uploaded = (glUnmapBuffer(GL_ARRAY_BUFFER) == GL_TRUE);
      }
    }
    if (!uploaded)
      glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, bucket.data());
  }

  mUploadedBuckets[lod] = bucket;
  ++frameStats.instanceUploads;
  frameStats.instanceUploadBytes += (int)std::min<size_t>(
      bytes, (size_t)std::numeric_limits<int>::max());
}

bool TerrainGpuRenderer::uploadChunk(const TerrainGpuChunkUpload &upload) {
  if (!mActive || upload.sampleCount != (uint32_t)mSampleEdge ||
      upload.heights.size() != (size_t)mSampleEdge * mSampleEdge ||
      upload.biomes.size() != (size_t)mSampleEdge * mSampleEdge) {
    return false;
  }

  const int64_t key = packKey(upload.cx, upload.cz);
  auto it = mChunks.find(key);
  int layer = -1;
  if (it == mChunks.end()) {
    if (mFreeLayers.empty())
      return false;
    layer = mFreeLayers.back();
    mFreeLayers.pop_back();
  } else {
    layer = it->second.layer;
  }

  glBindTexture(GL_TEXTURE_2D_ARRAY, mHeightPages);
  glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, mSampleEdge,
                  mSampleEdge, 1, GL_RED, GL_FLOAT, upload.heights.data());
  glBindTexture(GL_TEXTURE_2D_ARRAY, mBiomePages);
  glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, mSampleEdge,
                  mSampleEdge, 1, GL_RED, GL_FLOAT, upload.biomes.data());
  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

  if (glGetError() != GL_NO_ERROR) {
    if (it == mChunks.end())
      mFreeLayers.push_back(layer);
    return false;
  }

  ChunkRecord rec;
  rec.cx = upload.cx;
  rec.cz = upload.cz;
  rec.lod = std::clamp(upload.lod, 0, kMaxLodBuckets - 1);
  rec.layer = layer;
  rec.worldSize = upload.worldSize;
  rec.sampleCount = upload.sampleCount;
  rec.minHeight = upload.minHeight;
  rec.maxHeight = upload.maxHeight;
  mLastWorldSize = upload.worldSize;
  mChunks[key] = rec;
  ++mChunkRevision;
  invalidatePassCaches();
  return true;
}

void TerrainGpuRenderer::removeChunk(int cx, int cz) {
  const int64_t key = packKey(cx, cz);
  auto it = mChunks.find(key);
  if (it == mChunks.end())
    return;
  if (it->second.layer >= 0)
    mFreeLayers.push_back(it->second.layer);
  mChunks.erase(it);
  ++mChunkRevision;
  invalidatePassCaches();
}

void TerrainGpuRenderer::updateChunkLod(int cx, int cz, int lod) {
  auto it = mChunks.find(packKey(cx, cz));
  if (it != mChunks.end()) {
    it->second.lod = std::clamp(lod, 0, kMaxLodBuckets - 1);
    ++mChunkRevision;
    invalidatePassCaches();
  }
}

void TerrainGpuRenderer::extractFrustum(const glm::mat4 &vp) {
  const glm::vec4 r0(vp[0][0], vp[1][0], vp[2][0], vp[3][0]);
  const glm::vec4 r1(vp[0][1], vp[1][1], vp[2][1], vp[3][1]);
  const glm::vec4 r2(vp[0][2], vp[1][2], vp[2][2], vp[3][2]);
  const glm::vec4 r3(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);

  glm::vec4 raw[6] = {r3 + r0, r3 - r0, r3 + r1,
                      r3 - r1, r3 + r2, r3 - r2};
  for (int i = 0; i < 6; ++i) {
    const float len = glm::length(glm::vec3(raw[i]));
    mFrustumPlanes[i] = (len > 1e-5f) ? raw[i] / len : raw[i];
  }
}

bool TerrainGpuRenderer::sphereInFrustum(const glm::vec3 &center,
                                         float radius) const {
  for (const glm::vec4 &p : mFrustumPlanes) {
    if (glm::dot(glm::vec3(p), center) + p.w < -radius)
      return false;
  }
  return true;
}

TerrainGpuRendererStats TerrainGpuRenderer::render(
    Shader &shader, const glm::mat4 &viewProjection, const glm::vec3 &cameraPos,
    bool shadowPass, bool enableHorizonCulling, int horizonSectors) {
  if (!mActive)
    return {};

  extractFrustum(viewProjection);

  const int passIndex = shadowPass ? 1 : 0;
  PassCache &cache = mPassCache[passIndex];
  const uint64_t renderKey =
      buildRenderKey(cameraPos, shadowPass, enableHorizonCulling,
                     horizonSectors);
  const bool canReuseVisibleSet =
      cache.valid && cache.renderKey == renderKey &&
      cache.chunkRevision == mChunkRevision;

  TerrainGpuRendererStats frameStats;
  if (canReuseVisibleSet) {
    frameStats = cache.stats;
    for (int lod = 0; lod < kMaxLodBuckets; ++lod)
      mBuckets[lod] = cache.buckets[lod];
  } else {
    for (auto &bucket : mBuckets)
      bucket.clear();

    frameStats.active = true;
    frameStats.loadedChunks = (int)mChunks.size();
    frameStats.pagesUsed = mPageCapacity - (int)mFreeLayers.size();

    mCandidates.clear();
    mCandidates.reserve(mChunks.size());

    for (const auto &[_, rec] : mChunks) {
      const float ws = rec.worldSize;
      const glm::vec3 center(rec.cx * ws + ws * 0.5f,
                             (rec.minHeight + rec.maxHeight) * 0.5f,
                             rec.cz * ws + ws * 0.5f);
      const float verticalRadius =
          std::max(8.0f, std::abs(rec.maxHeight - rec.minHeight) * 0.5f);
      const float radius =
          std::sqrt(ws * ws * 0.5f + verticalRadius * verticalRadius);
      if (!sphereInFrustum(center, radius)) {
        ++frameStats.frustumCulledChunks;
        continue;
      }
      const glm::vec2 d(center.x - cameraPos.x, center.z - cameraPos.z);
      mCandidates.push_back({&rec, d.x * d.x + d.y * d.y});
    }

    std::sort(mCandidates.begin(), mCandidates.end(),
              [](const Candidate &a, const Candidate &b) {
                return a.distSq < b.distSq;
              });

    const bool useHorizon =
        enableHorizonCulling && !shadowPass && horizonSectors >= 32;
    const int sectors = std::clamp(horizonSectors, 32, 2048);
    if (useHorizon) {
      mHorizonScratch.assign(
          (size_t)sectors, -std::numeric_limits<float>::infinity());
    } else {
      mHorizonScratch.clear();
    }

    for (const Candidate &candidate : mCandidates) {
      const ChunkRecord &rec = *candidate.record;
      const float ws = rec.worldSize;
      const glm::vec2 d(rec.cx * ws + ws * 0.5f - cameraPos.x,
                        rec.cz * ws + ws * 0.5f - cameraPos.z);
      const float distXZ = std::max(0.001f, std::sqrt(d.x * d.x + d.y * d.y));

      if (useHorizon && distXZ > ws * 8.0f) {
        const float angle = wrapAngle(std::atan2(d.y, d.x));
        const float angularRadius = std::atan((ws * 0.75f) / distXZ);
        const float topAngle = std::atan2(rec.maxHeight - cameraPos.y, distXZ);
        const int centerSector =
            (int)std::floor(angle * (float)sectors / 6.28318530718f);
        const int radiusSectors =
            std::max(1, (int)std::ceil(angularRadius * (float)sectors /
                                       6.28318530718f));

        bool hidden = true;
        for (int s = centerSector - radiusSectors;
             s <= centerSector + radiusSectors; ++s) {
          if (mHorizonScratch[wrapIndex(s, sectors)] <= topAngle + 0.01745f) {
            hidden = false;
            break;
          }
        }
        if (hidden) {
          ++frameStats.horizonCulledChunks;
          continue;
        }
        for (int s = centerSector - radiusSectors;
             s <= centerSector + radiusSectors; ++s) {
          float &sector = mHorizonScratch[wrapIndex(s, sectors)];
          sector = std::max(sector, topAngle);
        }
      }

      TerrainGpuInstance inst;
      inst.chunk0 = glm::vec4(rec.cx * ws, rec.cz * ws, ws, (float)rec.layer);
      inst.chunk1 = glm::vec4((float)rec.sampleCount, (float)rec.lod,
                              rec.minHeight, rec.maxHeight);
      mBuckets[std::clamp(rec.lod, 0, kMaxLodBuckets - 1)].push_back(inst);
      ++frameStats.visibleChunks;
    }

    cache.valid = true;
    cache.renderKey = renderKey;
    cache.chunkRevision = mChunkRevision;
    cache.stats = frameStats;
    for (int lod = 0; lod < kMaxLodBuckets; ++lod)
      cache.buckets[lod] = mBuckets[lod];
  }

  shader.activate();
  shader.setBool("uGpuTerrainPass", true);
  shader.setBool("uInstanced", false);
  shader.setInt("uTerrainHeightPages", 14);
  shader.setInt("uTerrainBiomePages", 15);
  shader.setFloat("uTerrainSkirtDepth", std::max(1.0f, mLastWorldSize * 0.08f));
  if (!shadowPass) {
    shader.setBool("uTerrainPass", true);
    shader.setBool("uUseColor", false);
  }

  glActiveTexture(GL_TEXTURE14);
  glBindTexture(GL_TEXTURE_2D_ARRAY, mHeightPages);
  glActiveTexture(GL_TEXTURE15);
  glBindTexture(GL_TEXTURE_2D_ARRAY, mBiomePages);
  glActiveTexture(GL_TEXTURE0);

  for (int lod = 0; lod < kMaxLodBuckets; ++lod) {
    const auto &bucket = mBuckets[lod];
    if (bucket.empty() || mIndexCount[lod] == 0)
      continue;

    uploadBucketIfNeeded(lod, bucket, frameStats);

    GLStateCache::instance().bindVertexArray(mGridVao[lod]);
    glDrawElementsInstanced(GL_TRIANGLES, mIndexCount[lod], GL_UNSIGNED_INT,
                            nullptr, (GLsizei)bucket.size());
    ++frameStats.drawCalls;
  }

  GLStateCache::instance().bindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  if (!shadowPass)
    shader.setBool("uTerrainPass", false);
  shader.setBool("uGpuTerrainPass", false);

  mStats = frameStats;
  return frameStats;
}

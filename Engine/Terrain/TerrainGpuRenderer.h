#pragma once

#include "Rendering/RenderCapabilities.h"
#include "Rendering/Shader.h"

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct TerrainGpuChunkUpload {
  int cx = 0;
  int cz = 0;
  int lod = 0;
  float worldSize = 64.0f;
  uint32_t sampleCount = 0;
  std::vector<float> heights;
  std::vector<float> biomes;
  float minHeight = 0.0f;
  float maxHeight = 0.0f;
};

struct TerrainGpuRendererStats {
  int loadedChunks = 0;
  int visibleChunks = 0;
  int frustumCulledChunks = 0;
  int horizonCulledChunks = 0;
  int drawCalls = 0;
  int pagesUsed = 0;
  int instanceUploads = 0;
  int instanceUploadSkips = 0;
  int instanceUploadBytes = 0;
  bool active = false;
  bool fallback = false;
};

class TerrainGpuRenderer {
public:
  TerrainGpuRenderer() = default;
  ~TerrainGpuRenderer();

  bool initialize(int chunkSize, int pageCapacity);
  void shutdown();
  void clear();

  bool active() const { return mActive; }
  void setSubmissionBackend(RenderSubmissionBackend backend) {
    mSubmissionBackend = backend;
  }
  bool uploadChunk(const TerrainGpuChunkUpload &upload);
  void removeChunk(int cx, int cz);
  void updateChunkLod(int cx, int cz, int lod);

  TerrainGpuRendererStats render(Shader &shader, const glm::mat4 &viewProjection,
                                 const glm::vec3 &cameraPos, bool shadowPass,
                                 bool enableHorizonCulling,
                                 int horizonSectors);

  const TerrainGpuRendererStats &stats() const { return mStats; }

private:
  struct TerrainGpuInstance {
    glm::vec4 chunk0; // originX, originZ, worldSize, texture layer
    glm::vec4 chunk1; // sampleCount, lod, minHeight, maxHeight
  };

  struct ChunkRecord {
    int cx = 0;
    int cz = 0;
    int lod = 0;
    int layer = -1;
    float worldSize = 64.0f;
    uint32_t sampleCount = 0;
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
  };

  struct Candidate {
    const ChunkRecord *record = nullptr;
    float distSq = 0.0f;
  };

  static constexpr int kMaxLodBuckets = 5;

  struct PassCache {
    bool valid = false;
    uint64_t renderKey = 0;
    uint64_t chunkRevision = 0;
    TerrainGpuRendererStats stats{};
    std::vector<TerrainGpuInstance> buckets[kMaxLodBuckets];
  };

  bool buildGrid(int lod);
  static int64_t packKey(int cx, int cz);
  bool sphereInFrustum(const glm::vec3 &center, float radius) const;
  void extractFrustum(const glm::mat4 &viewProjection);
  uint64_t buildRenderKey(const glm::vec3 &cameraPos, bool shadowPass,
                          bool enableHorizonCulling, int horizonSectors) const;
  void invalidatePassCaches();
  bool bucketMatchesUploaded(int lod,
                             const std::vector<TerrainGpuInstance> &bucket) const;
  void uploadBucketIfNeeded(int lod, const std::vector<TerrainGpuInstance> &bucket,
                            TerrainGpuRendererStats &frameStats);

  bool mActive = false;
  int mChunkSize = 0;
  int mSampleEdge = 0;
  int mPageCapacity = 0;
  float mLastWorldSize = 64.0f;
  uint64_t mChunkRevision = 1;
  RenderSubmissionBackend mSubmissionBackend =
      RenderSubmissionBackend::Direct;

  GLuint mHeightPages = 0;
  GLuint mBiomePages = 0;
  GLuint mGridVao[kMaxLodBuckets] = {};
  GLuint mGridVbo[kMaxLodBuckets] = {};
  GLuint mGridEbo[kMaxLodBuckets] = {};
  GLuint mInstanceVbo[kMaxLodBuckets] = {};
  GLsizei mIndexCount[kMaxLodBuckets] = {};

  glm::vec4 mFrustumPlanes[6] = {};
  std::unordered_map<int64_t, ChunkRecord> mChunks;
  std::vector<int> mFreeLayers;
  std::vector<Candidate> mCandidates;
  std::vector<float> mHorizonScratch;
  std::vector<TerrainGpuInstance> mBuckets[kMaxLodBuckets];
  std::vector<TerrainGpuInstance> mUploadedBuckets[kMaxLodBuckets];
  size_t mInstanceVboCapacity[kMaxLodBuckets] = {};
  PassCache mPassCache[2];
  TerrainGpuRendererStats mStats{};
};

#pragma once
// TerrainSystem.h — Advanced chunk-based procedural terrain with biome system,
// multi-type vegetation, rock spawning, water planes, and terrain queries API.

#include "Assets/AssetManager.h"
#include "Assets/OBJModel.h"
#include "Core/PerlinNoise.h"
#include "ECS/Components.h"
#include "ECS/Registry.h"
#include "Scene/Scene.h"

#include <functional>
#include <future>
#include <glm/glm.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations
class PhysicsSystem;

// ─── Biome Types ───
enum class BiomeType : int {
  Ocean = 0,
  Plains = 1,
  Forest = 2,
  Desert = 3,
  Mountains = 4,
  Tundra = 5,
  COUNT = 6
};

// ─── Vegetation Types ───
enum class TreeType { Pine, Oak, Birch, DeadTree, Cactus };

// ─── Terrain Statistics ───
struct TerrainStats {
  int loadedChunks = 0;
  int totalTreeEntities = 0;
  int totalRockEntities = 0;
  int totalWaterPlanes = 0;
  int verticesGenerated = 0;
  int trianglesGenerated = 0;
  int biomeCounts[6] = {}; // Count of chunks per biome (dominant biome)
};

struct TerrainSettings {
  bool enabled = false;
  uint32_t seed = 42;
  int chunkSize = 32;        // vertices per chunk edge
  float heightScale = 10.0f; // max height amplitude
  float noiseFrequency = 0.02f;
  int viewDistance = 3;         // chunk radius around camera
  float chunkWorldSize = 64.0f; // world-space size of one chunk
  bool useRidgeNoise = false;
  bool singleBiomeOnly = true;  // force plains-only biome for now
  int octaves = 5;
  float lacunarity = 2.0f;
  float gain = 0.5f;
  float treeDensity = 0.15f; // forest tree density
  float biomeScale = 0.004f; // biome region frequency
  float seaLevel = -2.0f;
  float rockDensity = 0.2f;                 // mountain/tundra rock density
  float grassDensity = 0.25f;               // plains grass cluster density
  float rockScale = 0.7f;                   // uniform rock size multiplier
  float grassScale = 1.0f;                  // uniform grass size multiplier
  bool spawnWater = true;                   // generate water planes in ocean
  bool spawnRocks = true;                   // generate rocks in mountains
  bool spawnVegetation = true;              // generate trees/grass/cacti
  std::string customTreeModelPath = "";     // Path to custom .obj or .fbx
  std::string customRockModelPath = "";     // Path to custom .obj or .fbx
  std::string customGrassModelPath = "";    // Path to custom .obj or .fbx
  bool flipCustomGrass = false;             // Fix inverted objects
  std::string customFlowerModelPath = "";   // Path to custom .obj or .fbx
  std::string customCactusModelPath = "";   // Path to custom .obj or .fbx
  std::string customDeadTreeModelPath = ""; // Path to custom .obj or .fbx
};

class TerrainSystem {
public:
  void init(const TerrainSettings &settings, Scene &scene,
            AssetManager *assets);
  void applySettings(const TerrainSettings &settings);
  void update(const glm::vec3 &cameraPos);
  void regenerate();
  void shutdown();
  bool chopTree(EntityId treeEntity);
  // Must be called once per frame on main thread to GPU-upload completed chunks
  void flushPendingChunks();

  // Plug in physics system to enable automatic terrain collision
  void setPhysicsSystem(PhysicsSystem *ps) { mPhysicsSystem = ps; }

  bool isEnabled() const { return mSettings.enabled; }
  const TerrainStats &stats() const { return mStats; }
  // Move a prefab instance by delta (updates instanced mesh transform).
  bool movePrefabInstance(const std::string &prefabName, size_t instanceIndex,
                          const glm::vec3 &delta);
  bool getPrefabInstanceMatrix(const std::string &prefabName,
                               size_t instanceIndex, glm::mat4 &out) const;
  bool setPrefabInstanceMatrix(const std::string &prefabName,
                               size_t instanceIndex, const glm::mat4 &m);
  bool convertTreeToEntity(EntityId treeEntity);
  bool applyHeightBrush(const glm::vec3 &center, float radius, float delta);
  bool applyVegetationBrush(const glm::vec3 &center, float radius,
                            const std::string &prefabName, bool add,
                            int count);

  // ── Terrain Queries API ──
  float getHeightAt(float worldX, float worldZ) const;
  BiomeType getBiomeAt(float worldX, float worldZ) const;
  glm::vec3 getNormalAt(float worldX, float worldZ) const;
  float getSlopeAt(float worldX, float worldZ) const;
  bool isUnderwater(float worldX, float worldZ) const;
  bool isChunkLoadedAt(float worldX, float worldZ) const;
  glm::vec2 clampXZToLoadedRegion(float worldX, float worldZ,
                                  float margin = 1.0f) const;

private:
  struct ChunkCoord {
    int x, z;
    bool operator==(const ChunkCoord &o) const { return x == o.x && z == o.z; }
  };

  struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord &c) const {
      auto h1 = std::hash<int>()(c.x);
      auto h2 = std::hash<int>()(c.z);
      return h1 ^ (h2 << 16);
    }
  };

  struct ChunkData {
    EntityId entity = 0;
    std::string terrainAssetId;
    int terrainLod = 0;

    // Track how many instances of each prefab this chunk spawned
    std::unordered_map<std::string, int> prefabInstanceCounts;
    // Exact instance matrices for this chunk (for rebuilds)
    std::unordered_map<std::string, std::vector<glm::mat4>>
        prefabInstanceMatrices;
    // Tree entities per prefab (if any)
    std::unordered_map<std::string, std::vector<uint32_t>>
        prefabInstanceEntities;

    EntityId waterEntity = 0;
    std::string waterAssetId;
    BiomeType dominantBiome = BiomeType::Plains;
    int treeCount = 0;
    int rockCount = 0;
    // Jolt body ID for this chunk's HeightFieldShape (0xFFFFFFFF = none)
    uint32_t physicsBodyId = 0xFFFFFFFF;
  };

  struct HeightOffsetData {
    uint32_t sampleCount = 0;
    std::vector<float> offsets;
  };

  struct PaintedInstanceData {
    std::unordered_map<std::string, std::vector<glm::mat4>> prefabMatrices;
  };

  struct PrefabData {
    EntityId entity = 0;
    std::string assetId;
    bool runtimeAsset = false;
    float autoScale = 1.0f;           // uniform scale baked into every instance
    glm::vec3 baseRot = glm::vec3(0); // extra rotation (radians) per instance
  };
  std::unordered_map<std::string, PrefabData> mPrefabs;

  // Biome determination
  BiomeType getBiome(float worldX, float worldZ) const;
  BiomeType getChunkDominantBiome(int cx, int cz) const;

  // Height computation
  float sampleHeight(float worldX, float worldZ) const;
  float shapeBiomeHeight(BiomeType biome, float baseH, float ridgeH) const;
  float sampleBaseNoise(float worldX, float worldZ) const;
  float sampleRidgeNoise(float worldX, float worldZ) const;

  // Mesh generation
  std::vector<OBJModel::VertexData> generateChunkMesh(int cx, int cz);
  std::vector<OBJModel::VertexData> generateChunkMeshLod(int cx, int cz,
                                                         int lod);
  std::vector<OBJModel::VertexData> generateWaterPlane(int cx, int cz);

  // Vegetation generation
  void spawnVegetation(int cx, int cz, ChunkData &chunk);
  void spawnTreesForest(int cx, int cz, ChunkData &chunk);
  void spawnRocksMountain(int cx, int cz, ChunkData &chunk);
  void spawnDesertCacti(int cx, int cz, ChunkData &chunk);
  void spawnTundraDecor(int cx, int cz, ChunkData &chunk);
  void spawnPlainsGrass(int cx, int cz, ChunkData &chunk);

  // Chunk management
  void loadChunk(int cx, int cz);
  void unloadChunk(int cx, int cz);
  std::vector<ChunkCoord> getChunksByDistance(int cx, int cz, int radius) const;

  // Entity helpers
  void initPrefabs();
  void clearPrefabs();
  void addPrefabFromVerts(const std::string &name,
                          const std::vector<OBJModel::VertexData> &verts);
  size_t addPrefabInstance(const std::string &name, const glm::vec3 &pos,
                           const glm::vec3 &scale, const glm::vec3 &rot,
                           ChunkData &chunk);
  void registerTreeInstance(const std::string &prefabName, size_t instanceIndex,
                            const glm::vec3 &pos, const glm::vec3 &scale,
                            int cx, int cz, ChunkData *chunk = nullptr);
  void removeLastPrefabInstances(const std::string &prefabName, size_t count);
  void rebuildChunkTerrain(int cx, int cz, bool rebuildPhysics = true);
  void rebuildPrefabInstances(const std::string &prefabName);
  int computeChunkLod(int cameraChunkX, int cameraChunkZ, int chunkX,
                      int chunkZ) const;
  int lodResolution(int lod) const;

  TerrainSettings mSettings;
  TerrainStats mStats;
  PerlinNoise mNoise{0};
  PerlinNoise mTempNoise{0};
  PerlinNoise mMoistNoise{0};
  PerlinNoise mTreeNoise{0};
  PerlinNoise mRockNoise{0};   // Rock scatter noise
  PerlinNoise mDetailNoise{0}; // Fine detail noise
  Scene *mScene = nullptr;
  class AssetManager *mAssets = nullptr;
  PhysicsSystem *mPhysicsSystem =
      nullptr; // optional; enables terrain collision
  std::unordered_map<ChunkCoord, ChunkData, ChunkCoordHash> mChunks;
  std::unordered_map<ChunkCoord, HeightOffsetData, ChunkCoordHash>
      mHeightOffsets;
  std::unordered_map<ChunkCoord, PaintedInstanceData, ChunkCoordHash>
      mPaintedInstances;
  ChunkCoord mLastCameraChunk = {INT_MAX, INT_MAX};
  std::unordered_map<std::string, std::vector<uint32_t>>
      mPrefabInstanceEntities;

  // ── Async chunk generation ──────────────────────────────────
  // Result produced off the main thread (pure CPU work)
  struct PendingChunk {
    int cx, cz;
    int terrainLod = 0;
    std::vector<OBJModel::VertexData> terrainVerts;
    std::vector<OBJModel::VertexData> waterVerts;
    bool hasWater{false};
    BiomeType dominantBiome{BiomeType::Plains};
    // Per-prefab instance matrices computed off-thread
    std::unordered_map<std::string, std::vector<glm::mat4>> instanceMatrices;
    // Flat height array for HeightFieldShape (row-major, (sampleCount+1)^2)
    std::vector<float> heightSamples;
    uint32_t heightSampleCount{0}; // grid edge length (sampleCount+1)
  };
  // Results ready for GPU upload (written by worker, flushed on main thread)
  std::vector<PendingChunk> mPendingReady;
  std::mutex mPendingMutex;
  // In-flight futures (kept alive until done)
  std::vector<std::future<void>> mChunkFutures;
  // Track which chunks are already being generated so we don't double-queue
  std::unordered_map<ChunkCoord, bool, ChunkCoordHash> mInFlight;

  void loadChunkAsync(int cx, int cz, int lod);
};

#pragma once
// TerrainChunkManager.h — chunk streaming state machine + worker pool.
// Engine-core, no Vulkan/ECS/physics dependency: produces plain MeshData for
// the caller (VkTerrainSubsystem) to upload to the GPU and wire into the
// scene. No engine-object mutation happens inside a worker job; jobs only
// read TerrainSettings (by value) and TerrainNoiseSet (const ref, stateless
// per call -- safe to share read-only across threads).
//
// Phase 2 scope: streaming + LOD. Phase 3 adds vegetation scattering into the
// same worker job (see workerLoop()). Phase 4 adds sampleCollisionHeights()
// (see below) but stays physics-free otherwise -- collision-radius
// eligibility is re-evaluated by the caller (VkTerrainSubsystem) every frame
// against the current camera distance, not tied to job dispatch. Phase 5
// adds applyHeightBrush(): mutates the owned HeightOffsetGrid and forces a
// rebuild of any already-`ReadyToUpload` chunk it touches (bypassing the
// Unloaded-only dispatch gate normal streaming uses).

#include "HeightOffsetGrid.h"
#include "MeshData.h"
#include "ScatterManifest.h"
#include "TerrainNoise.h"
#include "TerrainScatter.h"
#include "TerrainSettings.h"
#include "TerrainTypes.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

class TerrainChunkManager {
public:
  TerrainChunkManager() = default;
  ~TerrainChunkManager();

  TerrainChunkManager(const TerrainChunkManager &) = delete;
  TerrainChunkManager &operator=(const TerrainChunkManager &) = delete;

  // Seeds the noise set and starts the worker pool. Safe to call once.
  // `manifest` is copied (read-only shared across worker threads, same
  // contract as `settings`) -- R6's Scatter panel will call init() again
  // (via regenerate()) when the user edits it.
  void init(const TerrainSettings &settings, ScatterManifest manifest = defaultScatterManifest());

  // Drains/joins worker threads. Safe to call even if init() wasn't.
  void shutdown();

  // Call once per frame. Recomputes the desired chunk set on chunk-boundary
  // crossings; dispatches build jobs EVERY call (nearest-first, up to
  // maxChunkLoadsPerUpdate per call -- new loads and LOD-band refreshes
  // share the budget) and drains completed/unload queues. Dispatching every
  // frame matters: keying dispatch to boundary crossings meant a stationary
  // camera never filled its view radius and large radii took kilometers of
  // travel to stream in.
  void streamUpdate(glm::vec3 cameraWorldPos);

  struct PendingUpload {
    ChunkCoord coord;
    int lod = 0;
    uint32_t jobGeneration = 0;
    MeshData mesh;
    // Chunk-average biome weights (mean over the sampled ground-field grid,
    // not a majority-vote single category -- there is no single category
    // any more, see BiomeWeights). Exposed for inspection/statistics; actual
    // placement (scatterLayers()) samples biome weights per-candidate, not
    // from this average.
    BiomeWeights dominantWeights;
    std::vector<ScatterInstance> scatter;
  };

  // Returns and clears chunks whose mesh is ready to be uploaded this frame.
  std::vector<PendingUpload> takePendingUploads();

  // Returns and clears chunks the caller should tear down its render-side
  // state for (destroy ECS entity, stop instancing -- GPU mesh reuse is the
  // caller's own concern, see the mesh-cache note in VkTerrainSubsystem).
  std::vector<ChunkCoord> takePendingUnloads();

  const TerrainSettings &settings() const { return mSettings; }
  // Mutable access, for the one value that can only be known after init():
  // TerrainSettings::seaLevel, which resolveSeaLevel() derives from the
  // initialised noise set. Must be set before any chunk builds -- every
  // consumer (carve, scatter waterline, renderer water field) reads it.
  TerrainSettings &settingsMutable() { return mSettings; }

  // Synchronously samples fixed-LOD0 (chunkResolution) heights for a
  // chunk's collision shape, independent of whatever LOD that chunk's
  // visual mesh is currently using (a chunk within collisionChunkRadius may
  // already be rendering coarser than LOD0). Safe to call from the main
  // thread even while worker threads are running -- sampleHeightGrid()/the
  // noise set are read-only per call, same contract the async jobs already
  // rely on. A full chunkResolution^2 sample costs tens of milliseconds:
  // frame-loop callers must use the async request/take pair below instead
  // (this stays for tests/tools).
  void sampleCollisionHeights(ChunkCoord coord,
                              std::vector<float> &outHeights) const;

  // Async collision sampling on the worker pool (collision jobs jump ahead
  // of mesh jobs -- a missing heightfield under the player matters more
  // than a distant chunk's visuals). Returns true if newly queued, false
  // if that coord is already queued/in flight (deduped). Results arrive
  // via takeCompletedCollision(); a coord may be re-requested after its
  // result was taken (brush refresh re-samples).
  bool requestCollisionHeights(ChunkCoord coord);

  struct CollisionResult {
    ChunkCoord coord;
    std::vector<float> heights; // chunkResolution^2, row-major
  };
  std::vector<CollisionResult> takeCompletedCollision();

  // Read access for TerrainQuery (constructed by the caller, e.g.
  // VkTerrainSubsystem, referencing these two plus settings() -- all stable
  // for this manager's lifetime).
  const TerrainNoiseSet &noiseSet() const { return *mNoiseSet; }
  // Mutable access for the one-time island segmentation at world creation
  // (TerrainIslands). Must happen before any chunk builds; after that the
  // noise set is read-only shared state across worker threads.
  TerrainNoiseSet &noiseSetMutable() { return *mNoiseSet; }
  const HeightOffsetGrid &editsGrid() const { return mEdits; }

  // Paints a height-offset brush stroke (radial linear falloff, `strength`
  // negated internally when `lower` is true), then forces a rebuild of
  // every touched chunk that's currently `ReadyToUpload` (already built and
  // rendering) -- bypassing the Unloaded-only dispatch gate normal
  // streaming uses. Touched chunks not currently in that state (unloaded,
  // or already mid-rebuild) just get the edit stored silently in the
  // offset grid; every call site that samples heights already threads
  // `editsGrid()` through, so it's picked up automatically whenever that
  // chunk next builds. Main-thread only.
  void applyHeightBrush(glm::vec2 worldXZ, float radius, float strength,
                        bool lower);

  // Exposed for testing/inspection (Statistics panel, unit tests).
  size_t trackedChunkCount() const;

  struct Job {
    ChunkCoord coord;
    int lod;
    // Chebyshev distance in chunks from the camera chunk this job was
    // generated for. `lod` is derived from it, but scatter needs the raw
    // value too: LOD bands are relative to viewDistanceChunks (so LOD0
    // covers a much larger area at a large view distance), whereas grass's
    // radius cap and distance falloff are absolute. Carried on the job
    // because the worker cannot ask "where is the camera now" -- it must use
    // the distance the job was dispatched at, or chunks built at different
    // times would disagree.
    int chebyshev = 0;
    uint32_t jobGeneration = 0;
  };

  // Pure helpers, public and static so tests can exercise the streaming
  // math without spinning up the worker pool / Vulkan / ECS.
  // Chebyshev-disc coords around `center` out to `radius`, nearest-first,
  // each paired with its assigned LOD band.
  static std::vector<Job> desiredChunkSet(ChunkCoord center, int radius);
  // LOD band (0..4) from Chebyshev distance.
  static int lodForDistance(int chebyshevDist, int viewDistanceChunks);
  static uint32_t lodSamplesPerEdge(uint32_t baseResolution, int lod);

private:
  void workerLoop();
  void enqueueJob(const Job &job);

  TerrainSettings mSettings;
  ScatterManifest mManifest;
  std::unique_ptr<TerrainNoiseSet> mNoiseSet;
  HeightOffsetGrid mEdits;

  // Per-chunk streaming state. builtLod is the LOD of the last COMPLETED
  // build (-1 = none); desiredLod tracks the current distance band, updated
  // on boundary crossings -- a mismatch on a ReadyToUpload chunk triggers a
  // re-mesh (previously a chunk kept its birth LOD forever: approached
  // chunks stayed blobby, receding ones kept full-detail meshes + scatter).
  struct TrackedChunk {
    ChunkState state = ChunkState::Unloaded;
    int builtLod = -1;
    int desiredLod = 0;
    // Mirrors desiredLod's update: kept so a brush-triggered rebuild (which
    // has no Job to copy from) can re-scatter at the chunk's current
    // distance instead of assuming 0, which would make an edited far chunk
    // sprout full-density grass.
    int chebyshev = 0;
    uint32_t jobGeneration = 0;
    bool dirtyAfterBuild = false;
  };
  std::unordered_map<ChunkCoord, TrackedChunk> mChunks;
  // Nearest-first desired set (with per-coord LOD bands) from the last
  // boundary crossing; the per-frame dispatch pass walks it.
  std::vector<Job> mDesiredJobs;
  ChunkCoord mLastCameraChunk{INT32_MIN, INT32_MIN};
  bool mHasCameraChunk = false;

  // Worker pool.
  std::vector<std::thread> mWorkers;
  std::atomic<bool> mStopWorkers{false};
  std::mutex mJobMutex;
  std::condition_variable mJobCv;
  std::deque<Job> mJobQueue;
  // Collision-only jobs (guarded by mJobMutex alongside mJobQueue; popped
  // preferentially by workers). mCollisionPending dedups queued/in-flight
  // coords; entries leave it when the worker publishes the result.
  std::deque<ChunkCoord> mCollisionQueue;
  std::unordered_set<ChunkCoord> mCollisionPending;

  std::mutex mCompletedMutex;
  std::deque<PendingUpload> mCompletedQueue;
  std::vector<CollisionResult> mCollisionDone; // guarded by mCompletedMutex

  // Drained from mCompletedQueue by streamUpdate() (main thread only), held
  // here until takePendingUploads() is called -- no locking needed since
  // both are only ever touched from the main thread.
  std::vector<PendingUpload> mUploadOut;

  std::mutex mUnloadMutex;
  std::deque<ChunkCoord> mUnloadQueue;
};

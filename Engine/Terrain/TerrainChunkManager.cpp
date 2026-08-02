#include "TerrainChunkManager.h"

#include "TerrainChunkMesher.h"

#include <algorithm>
#include <array>
#include <tuple>

TerrainChunkManager::~TerrainChunkManager() { shutdown(); }

void TerrainChunkManager::init(const TerrainSettings &settings, ScatterManifest manifest) {
  mSettings = settings;
  mManifest = std::move(manifest);
  mNoiseSet = std::make_unique<TerrainNoiseSet>(settings.seed);
  // Re-init means "fresh start" -- harmless no-op on first-ever startup,
  // but makes a full regenerate (shutdown()+init() with new settings) wipe
  // brush edits automatically rather than relying on the caller to
  // remember, since old edits would otherwise be geometrically desynced or
  // artistically meaningless against a newly-generated terrain shape.
  mEdits.clear();

  mStopWorkers = false;
  const uint32_t n = std::max(1u, settings.workerThreads);
  mWorkers.reserve(n);
  for (uint32_t i = 0; i < n; ++i)
    mWorkers.emplace_back([this] { workerLoop(); });
}

void TerrainChunkManager::shutdown() {
  mStopWorkers = true;
  mJobCv.notify_all();
  for (std::thread &t : mWorkers)
    if (t.joinable())
      t.join();
  mWorkers.clear();

  {
    std::lock_guard<std::mutex> lock(mJobMutex);
    mJobQueue.clear();
    mCollisionQueue.clear();
    mCollisionPending.clear();
  }
  {
    std::lock_guard<std::mutex> lock(mCompletedMutex);
    mCompletedQueue.clear();
    mCollisionDone.clear();
  }
  {
    std::lock_guard<std::mutex> lock(mUnloadMutex);
    mUnloadQueue.clear();
  }
  mChunks.clear();
  mDesiredJobs.clear();
  mNoiseSet.reset();
  mHasCameraChunk = false;
}

uint32_t TerrainChunkManager::lodSamplesPerEdge(uint32_t baseResolution, int lod) {
  if (baseResolution < 2)
    return baseResolution;
  const uint32_t base = baseResolution - 1;
  const uint32_t shifted = base >> static_cast<uint32_t>(std::max(0, lod));
  return std::max(2u, shifted) + 1u;
}

int TerrainChunkManager::lodForDistance(int chebyshevDist, int viewDistanceChunks) {
  // 5 LOD bands (0..4) spread across the view distance, nearest = finest.
  if (viewDistanceChunks <= 0)
    return 0;
  const float t = static_cast<float>(chebyshevDist) /
                  static_cast<float>(viewDistanceChunks);
  if (t <= 0.2f) return 0;
  if (t <= 0.4f) return 1;
  if (t <= 0.6f) return 2;
  if (t <= 0.8f) return 3;
  return 4;
}

std::vector<TerrainChunkManager::Job>
TerrainChunkManager::desiredChunkSet(ChunkCoord center, int radius) {
  std::vector<Job> jobs;
  if (radius < 0)
    return jobs;
  jobs.reserve(static_cast<size_t>(2 * radius + 1) * (2 * radius + 1));
  for (int dz = -radius; dz <= radius; ++dz) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int chebyshev = std::max(std::abs(dx), std::abs(dz));
      Job job;
      job.coord = ChunkCoord{center.x + dx, center.z + dz};
      job.lod = lodForDistance(chebyshev, radius);
      job.chebyshev = chebyshev;
      jobs.push_back(job);
    }
  }
  std::sort(jobs.begin(), jobs.end(), [center](const Job &a, const Job &b) {
    const int64_t da = static_cast<int64_t>(a.coord.x - center.x) * (a.coord.x - center.x) +
                        static_cast<int64_t>(a.coord.z - center.z) * (a.coord.z - center.z);
    const int64_t db = static_cast<int64_t>(b.coord.x - center.x) * (b.coord.x - center.x) +
                        static_cast<int64_t>(b.coord.z - center.z) * (b.coord.z - center.z);
    return da < db;
  });
  return jobs;
}

void TerrainChunkManager::enqueueJob(const Job &job) {
  {
    std::lock_guard<std::mutex> lock(mJobMutex);
    mJobQueue.push_back(job);
  }
  mJobCv.notify_one();
}

void TerrainChunkManager::workerLoop() {
  while (true) {
    Job job;
    bool haveCollision = false;
    ChunkCoord collisionCoord{};
    {
      std::unique_lock<std::mutex> lock(mJobMutex);
      mJobCv.wait(lock, [this] {
        return mStopWorkers || !mJobQueue.empty() || !mCollisionQueue.empty();
      });
      if (mStopWorkers && mJobQueue.empty() && mCollisionQueue.empty())
        return;
      // Collision first: the physics ground under the player matters more
      // than a distant chunk's visual mesh.
      if (!mCollisionQueue.empty()) {
        haveCollision = true;
        collisionCoord = mCollisionQueue.front();
        mCollisionQueue.pop_front();
      } else {
        job = mJobQueue.front();
        mJobQueue.pop_front();
      }
    }

    if (haveCollision) {
      CollisionResult result;
      result.coord = collisionCoord;
      const glm::vec2 minCorner =
          chunkMinCorner(collisionCoord, mSettings.chunkWorldSize);
      sampleHeightGrid(*mNoiseSet, mSettings, minCorner,
                       mSettings.chunkWorldSize, mSettings.chunkResolution,
                       &mEdits, result.heights, /*outFields=*/nullptr);
      {
        std::lock_guard<std::mutex> lock(mCompletedMutex);
        mCollisionDone.push_back(std::move(result));
      }
      {
        // Cleared AFTER publishing so a re-request between publish and
        // main-thread drain queues a fresh sample rather than being lost.
        std::lock_guard<std::mutex> lock(mJobMutex);
        mCollisionPending.erase(collisionCoord);
      }
      continue;
    }

    const uint32_t samplesPerEdge =
        lodSamplesPerEdge(mSettings.chunkResolution, job.lod);
    const glm::vec2 minCorner = chunkMinCorner(job.coord, mSettings.chunkWorldSize);

    std::vector<float> heights;
    std::vector<TerrainGroundFields> fields;
    sampleHeightGrid(*mNoiseSet, mSettings, minCorner, mSettings.chunkWorldSize,
                      samplesPerEdge, &mEdits, heights, &fields);

    MeshData mesh = buildTerrainChunkMesh(heights, fields, samplesPerEdge,
                                          mSettings.chunkWorldSize,
                                          mSettings.outcropThreshold);

    // Chunk-average biome weights: mean over the sampled ground-field grid
    // (there's no single dominant category any more -- scatterVegetation
    // gates each species layer by these continuous weights instead).
    BiomeWeights avgWeights;
    if (!fields.empty()) {
      double sumMeadow = 0.0, sumForest = 0.0, sumMountain = 0.0;
      for (const TerrainGroundFields &f : fields) {
        sumMeadow += f.wMeadow;
        sumForest += f.wForest;
        sumMountain += f.wMountain;
      }
      const double n = static_cast<double>(fields.size());
      avgWeights.meadow = static_cast<float>(sumMeadow / n);
      avgWeights.forest = static_cast<float>(sumForest / n);
      avgWeights.mountain = static_cast<float>(sumMountain / n);
    }

    PendingUpload result;
    result.coord = job.coord;
    result.lod = job.lod;
    result.mesh = std::move(mesh);
    result.dominantWeights = avgWeights;

    // Only scatter for the finest LOD -- distant chunks don't need
    // per-plant/rock detail (no impostor mesh in this design), so skipping
    // it there avoids wasted placement work for instances that would add
    // little visible value at typical view distances.
    if (job.lod == 0) {
      const uint32_t chunkSeed = chunkSeedFor(mSettings.seed, job.coord);
      scatterLayers(mManifest, *mNoiseSet, mSettings, minCorner,
                   mSettings.chunkWorldSize, chunkSeed, job.chebyshev,
                   result.scatter);
    }

    std::lock_guard<std::mutex> lock(mCompletedMutex);
    mCompletedQueue.push_back(std::move(result));
  }
}

void TerrainChunkManager::streamUpdate(glm::vec3 cameraWorldPos) {
  if (!mNoiseSet)
    return;

  const ChunkCoord currentChunk = chunkCoordFromWorldXZ(
      glm::vec2(cameraWorldPos.x, cameraWorldPos.z), mSettings.chunkWorldSize);

  // Boundary crossing: refresh the desired set + every tracked chunk's
  // desired LOD band, and mark no-longer-desired chunks for unload.
  if (!mHasCameraChunk || currentChunk != mLastCameraChunk) {
    mHasCameraChunk = true;
    mLastCameraChunk = currentChunk;

    mDesiredJobs = desiredChunkSet(currentChunk, mSettings.viewDistanceChunks);

    std::vector<ChunkCoord> desiredCoords;
    desiredCoords.reserve(mDesiredJobs.size());
    for (const Job &j : mDesiredJobs)
      desiredCoords.push_back(j.coord);
    std::sort(desiredCoords.begin(), desiredCoords.end(),
              [](const ChunkCoord &a, const ChunkCoord &b) {
                return std::tie(a.x, a.z) < std::tie(b.x, b.z);
              });

    for (auto it = mChunks.begin(); it != mChunks.end();) {
      const bool stillDesired = std::binary_search(
          desiredCoords.begin(), desiredCoords.end(), it->first,
          [](const ChunkCoord &a, const ChunkCoord &b) {
            return std::tie(a.x, a.z) < std::tie(b.x, b.z);
          });
      if (!stillDesired && it->second.state != ChunkState::Unloading) {
        std::lock_guard<std::mutex> lock(mUnloadMutex);
        mUnloadQueue.push_back(it->first);
        it->second.state = ChunkState::Unloading;
      }
      ++it;
    }

    for (const Job &j : mDesiredJobs) {
      auto it = mChunks.find(j.coord);
      if (it != mChunks.end()) {
        it->second.desiredLod = j.lod;
        it->second.chebyshev = j.chebyshev;
      }
    }
  }

  // Dispatch pass -- every frame, nearest-first, budgeted. Untracked coords
  // (never seen, or unloaded and since re-desired) start a fresh build;
  // idle chunks whose LOD band changed re-mesh at the new band.
  uint32_t dispatched = 0;
  for (const Job &j : mDesiredJobs) {
    if (dispatched >= mSettings.maxChunkLoadsPerUpdate)
      break;
    auto found = mChunks.find(j.coord);
    if (found == mChunks.end())
      found = mChunks.emplace(j.coord, TrackedChunk{ChunkState::Unloaded, -1,
                                                    j.lod}).first;
    TrackedChunk &tracked = found->second;
    const bool freshLoad = tracked.state == ChunkState::Unloaded;
    const bool lodRefresh = tracked.state == ChunkState::ReadyToUpload &&
                            tracked.builtLod != tracked.desiredLod;
    if (!freshLoad && !lodRefresh)
      continue;
    tracked.state = ChunkState::Building;
    tracked.chebyshev = j.chebyshev;
    enqueueJob(Job{j.coord, tracked.desiredLod, j.chebyshev});
    ++dispatched;
  }

  // Drain completed jobs into the upload queue, budgeted per frame. A chunk
  // can be marked Unloading while its build job is still in flight (camera
  // moved away before the worker finished); such stale results are dropped
  // here rather than uploaded, so a chunk never gets instanced right after
  // (or instead of) being torn down. Dropping stale entries doesn't count
  // against the per-frame upload budget.
  {
    std::lock_guard<std::mutex> lock(mCompletedMutex);
    uint32_t taken = 0;
    while (taken < mSettings.maxCompletedChunksPerFrame && !mCompletedQueue.empty()) {
      PendingUpload front = std::move(mCompletedQueue.front());
      mCompletedQueue.pop_front();

      auto found = mChunks.find(front.coord);
      if (found == mChunks.end() ||
          found->second.state != ChunkState::Building)
        continue; // stale: unloaded/superseded since this job was dispatched

      found->second.state = ChunkState::ReadyToUpload;
      found->second.builtLod = front.lod;
      mUploadOut.push_back(std::move(front));
      ++taken;
    }
  }
}

std::vector<TerrainChunkManager::PendingUpload>
TerrainChunkManager::takePendingUploads() {
  std::vector<PendingUpload> out;
  out.swap(mUploadOut);
  return out;
}

std::vector<ChunkCoord> TerrainChunkManager::takePendingUnloads() {
  std::vector<ChunkCoord> out;
  {
    std::lock_guard<std::mutex> lock(mUnloadMutex);
    out.assign(mUnloadQueue.begin(), mUnloadQueue.end());
    mUnloadQueue.clear();
  }
  for (const ChunkCoord &c : out)
    mChunks.erase(c);
  return out;
}

size_t TerrainChunkManager::trackedChunkCount() const { return mChunks.size(); }

void TerrainChunkManager::sampleCollisionHeights(
    ChunkCoord coord, std::vector<float> &outHeights) const {
  if (!mNoiseSet) {
    outHeights.clear();
    return;
  }
  const glm::vec2 minCorner = chunkMinCorner(coord, mSettings.chunkWorldSize);
  sampleHeightGrid(*mNoiseSet, mSettings, minCorner, mSettings.chunkWorldSize,
                   mSettings.chunkResolution, &mEdits, outHeights,
                   /*outFields=*/nullptr);
}

void TerrainChunkManager::applyHeightBrush(glm::vec2 worldXZ, float radius,
                                           float strength, bool lower) {
  mEdits.applyBrush(worldXZ, radius, lower ? -strength : strength,
                    mSettings.chunkWorldSize, mSettings.chunkResolution);

  for (const ChunkCoord &coord : mEdits.takeTouchedChunks()) {
    auto it = mChunks.find(coord);
    // Not loaded, or already (un)loading/mid-rebuild: the edit is already
    // stored in mEdits and will be picked up automatically whenever this
    // chunk next builds (every sampleHeightGrid call site above now threads
    // &mEdits through) -- nothing further to do here.
    if (it == mChunks.end() || it->second.state != ChunkState::ReadyToUpload)
      continue;

    it->second.state = ChunkState::Building;
    enqueueJob(Job{coord, it->second.desiredLod, it->second.chebyshev});
  }
}

bool TerrainChunkManager::requestCollisionHeights(ChunkCoord coord) {
  if (!mNoiseSet)
    return false;
  {
    std::lock_guard<std::mutex> lock(mJobMutex);
    if (!mCollisionPending.insert(coord).second)
      return false; // already queued or in flight
    mCollisionQueue.push_back(coord);
  }
  mJobCv.notify_one();
  return true;
}

std::vector<TerrainChunkManager::CollisionResult>
TerrainChunkManager::takeCompletedCollision() {
  std::vector<CollisionResult> out;
  std::lock_guard<std::mutex> lock(mCompletedMutex);
  out.swap(mCollisionDone);
  return out;
}

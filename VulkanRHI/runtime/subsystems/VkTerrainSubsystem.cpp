#include "VkTerrainSubsystem.h"

#include "../VkAppState.h"
#include "VulkanRenderer.h"

#include "Assets/AssetManager.h"
#include "ECS/Components.h"
#include "TerrainScatter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace {
// The camera far plane must reach past the actual streamed disc (radius =
// viewDistanceChunks * chunkWorldSize) or chunks stream in and cull
// correctly but are geometrically clipped before the fragment shader ever
// runs -- "render distance" would silently stop doing anything past the
// default's ~384m. +32m margin covers a chunk's own diagonal so its far
// edge doesn't clip exactly at the boundary.
float farPlaneForSettings(const TerrainSettings &s) {
  return static_cast<float>(s.viewDistanceChunks) * s.chunkWorldSize + 32.0f;
}

// R2 (MEADOW_TERRAIN_REVAMP_PLAN.md §5a): default texture sets for the
// 5-layer splat, picked from the repo's existing PBR asset library --
// nothing here needs new assets to demo. Order matches
// FrameDataGpu::terrainTexA/B/C/D exactly: meadow/forest/dirt/rock/scree.
void setDefaultTerrainMaterials(vkrhi::VulkanRenderer::Params &p,
                                const std::string &assetDir) {
  using Slot = vkrhi::VulkanRenderer::Params::TerrainMaterialSlot;
  const std::string m = assetDir + "/terraingeneratorassets/materials/textures/Mossy Ground_";
  const std::string f = assetDir + "/materials/m5/textures/ForestGround01_";
  const std::string d = assetDir + "/materials/m7/textures/T_GroundDirt_03_";
  const std::string r = assetDir + "/materials/m3/textures/rocky-rugged-terrain_1_";
  const std::string s = assetDir + "/materials/m6/textures/GrassyRocks01_";

  p.terrainMaterialSlots[0] = Slot{m + "basecolor.jpg", "", "", 6.0f};
  p.terrainMaterialSlots[1] = Slot{f + "BaseColor.jpg", "", "", 7.0f};
  p.terrainMaterialSlots[2] = Slot{d + "basecolor.jpg", "", "", 5.0f};
  p.terrainMaterialSlots[3] = Slot{r + "albedo.jpg", "", "", 9.0f};
  p.terrainMaterialSlots[4] = Slot{s + "BaseColor.jpg", "", "", 8.0f};
  p.terrainMaterialsDirty = true;
}

// R4: manifest mesh paths are written relative to the project root (e.g.
// "assets/terraingeneratorassets/tree.obj", per the plan's own §6a JSON
// example) -- assetDir is ".../assets", so its parent is that root.
std::string resolveScatterMeshPath(const std::string &assetDir, const std::string &meshPath) {
  namespace fs = std::filesystem;
  fs::path p(meshPath);
  if (p.is_absolute())
    return meshPath;
  fs::path root = fs::path(assetDir).parent_path();
  return (root / p).lexically_normal().string();
}

std::string lowerExt(const std::string &path) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// Dispatches to the right AssetManager parser by extension (plan §6a:
// ".obj and .fbx (and .gltf) all work today"). OBJ gets a corrective
// up-axis rotation (if the layer specifies one -- some source assets like
// tree.obj aren't authored Y-up) applied BEFORE recentering
// (MeshData::Recenter::BaseY, base of the mesh at y=0), so authored-off-
// origin assets like rock.obj (~53 units off in the repo) sit correctly
// once placed at a sampled ground height; the other parsers have no
// equivalent recenter/rotate step yet.
// `alreadyFixed` carries the set of paths whose one-time corrective
// rotate+recenter has already been applied THIS load pass.
//
// It has to exist because AssetManager::loadOBJ() caches by path and
// rotateOBJ()/recenterOBJ() mutate that shared record in place: two manifest
// layers pointing at one file (which R5's default manifest does deliberately
// -- pine_canopy and pine_young are both tree.obj) would otherwise rotate it
// twice, laying the second layer's trees on their backs. The corrective
// transform describes the ASSET's authoring convention, not the layer, so
// applying it exactly once per file is the correct reading either way.
const MeshData *loadScatterMeshData(AssetManager &assets, const std::string &resolvedPath,
                                    glm::vec3 upAxisFixDeg,
                                    std::unordered_set<std::string> &alreadyFixed) {
  const std::string ext = lowerExt(resolvedPath);
  if (ext == ".obj") {
    OBJHandle h = assets.loadOBJ(resolvedPath);
    if (!h.valid())
      return nullptr;
    if (alreadyFixed.insert(resolvedPath).second) {
      if (glm::dot(upAxisFixDeg, upAxisFixDeg) > 1e-8f)
        assets.rotateOBJ(h, upAxisFixDeg);
      assets.recenterOBJ(h, MeshData::Recenter::BaseY);
    }
    return assets.getOBJData(h);
  }
  if (ext == ".fbx") {
    UFBXHandle h = assets.loadUFBX(resolvedPath);
    return h.valid() ? assets.getUFBXData(h) : nullptr;
  }
  if (ext == ".gltf" || ext == ".glb") {
    GLTFHandle h = assets.loadGLTF(resolvedPath);
    return h.valid() ? assets.getGLTFData(h) : nullptr;
  }
  return nullptr;
}
} // namespace

bool VkTerrainSubsystem::initialize() {
  if (!mState.renderer)
    return false;

  setDefaultTerrainMaterials(mState.renderer->params(), mState.assetDir);

  // R4: load the scatter manifest from project-root terrain_scatter.json;
  // write the default one out if it doesn't exist yet so there's something
  // real for the user to edit (R6's Scatter panel), rather than only ever
  // living as an in-memory default.
  namespace fs = std::filesystem;
  const std::string manifestPath =
      (fs::path(mState.assetDir).parent_path() / "terrain_scatter.json").string();
  if (!loadScatterManifest(manifestPath, mManifest)) {
    mManifest = defaultScatterManifest();
    writeScatterManifest(manifestPath, mManifest);
  } else if (mManifest.version < kScatterManifestVersion) {
    // R5 migration. An R4-era terrain_scatter.json has no grass layers at
    // all and the old short tree proportions, so silently keeping it would
    // leave the new system looking broken ("I enabled grass and nothing
    // happened") with nothing pointing at the cause. The user's file is
    // preserved next to the new one rather than overwritten, since it may
    // contain hand-tuned layers.
    const std::string backupPath = manifestPath + ".v" +
                                   std::to_string(mManifest.version) + ".bak";
    std::error_code ec;
    fs::copy_file(manifestPath, backupPath,
                  fs::copy_options::overwrite_existing, ec);
    std::fprintf(stderr,
                 "[VkTerrainSubsystem] terrain_scatter.json is v%d (current is "
                 "v%d) -- regenerating defaults; previous file saved as %s\n",
                 mManifest.version, kScatterManifestVersion,
                 ec ? "<backup failed>" : backupPath.c_str());
    mManifest = defaultScatterManifest();
    writeScatterManifest(manifestPath, mManifest);
  }

  TerrainSettings settings; // defaults from TerrainSettings.h
  // Headless perf/verification override: lets smoke runs exercise large
  // view distances without touching the editor slider.
  if (const char *vd = std::getenv("GLGEN_VK_VIEWDIST")) {
    const int v = std::atoi(vd);
    if (v >= 1 && v <= 64)
      settings.viewDistanceChunks = v;
  }
  if (const char *hs = std::getenv("GLGEN_VK_HEIGHTSCALE")) {
    const float v = static_cast<float>(std::atof(hs));
    if (v > 0.0f)
      settings.heightScale = v;
  }
  mState.terrain.init(settings, mManifest);
  mQuery = std::make_unique<TerrainQuery>(mState.terrain.settings(),
                                          mState.terrain.noiseSet(),
                                          &mState.terrain.editsGrid());
  mState.renderer->params().farPlane = farPlaneForSettings(settings);

  loadLayerMeshes();

  mReady = true;
  return true;
}

void VkTerrainSubsystem::loadLayerMeshes() {
  mLayerMesh.assign(mManifest.layers.size(), UINT32_MAX);
  mLayerBoundsRadius.assign(mManifest.layers.size(), 0.5f);
  mLayerMeshHeight.assign(mManifest.layers.size(), 1.0f);

  bool addedMesh = false;
  // See loadScatterMeshData(): one corrective transform per FILE, not per
  // layer. Scoped to this call, matching the AssetManager cache's own reset
  // semantics on reload.
  std::unordered_set<std::string> fixedMeshPaths;
  // Diagnoses the one way sharing a mesh across layers can still go wrong:
  // two layers disagreeing about the file's up-axis convention. First one
  // wins (it is the asset's property, so they cannot both be right).
  std::unordered_map<std::string, glm::vec3> fixByPath;

  for (size_t i = 0; i < mManifest.layers.size(); ++i) {
    const ScatterLayer &layer = mManifest.layers[i];
    const std::string resolved = resolveScatterMeshPath(mState.assetDir, layer.meshPath);

    auto priorFix = fixByPath.find(resolved);
    if (priorFix != fixByPath.end() &&
        glm::length(priorFix->second - layer.meshUpAxisFixDeg) > 1e-4f) {
      std::fprintf(stderr,
                   "[VkTerrainSubsystem] scatter layer '%s': meshUpAxisFixDeg "
                   "disagrees with an earlier layer using '%s'; keeping the "
                   "first (%.0f,%.0f,%.0f)\n",
                   layer.name.c_str(), resolved.c_str(), priorFix->second.x,
                   priorFix->second.y, priorFix->second.z);
    } else {
      fixByPath.emplace(resolved, layer.meshUpAxisFixDeg);
    }

    const MeshData *data = loadScatterMeshData(mState.assets, resolved,
                                               layer.meshUpAxisFixDeg, fixedMeshPaths);
    if (!data || data->submeshes.empty()) {
      std::fprintf(stderr,
                   "[VkTerrainSubsystem] scatter layer '%s': failed to load mesh '%s'\n",
                   layer.name.c_str(), resolved.c_str());
      continue;
    }
    glm::vec3 bmin, bmax;
    if (data->getGlobalBounds(bmin, bmax)) {
      mLayerBoundsRadius[i] = glm::length(bmax - bmin) * 0.5f;
      // Bounds are post-recenter (MeshData::Recenter::BaseY puts the base at
      // y=0), so bmax.y IS the mesh height.
      mLayerMeshHeight[i] = std::max(bmax.y - std::min(bmin.y, 0.0f), 1e-3f);
    }
    char debugName[64];
    std::snprintf(debugName, sizeof(debugName), "ScatterLayer_%s", layer.name.c_str());
    mLayerMesh[i] = mState.renderer->createMeshFromData(*data, debugName);
    if (mLayerMesh[i] != UINT32_MAX)
      addedMesh = true;
  }
  if (addedMesh)
    mState.renderer->growScene();
}

void VkTerrainSubsystem::shutdown() {
  mReady = false;
  // Explicit cleanup for symmetry -- SubsystemManager tears down subsystems
  // in reverse init order, so VkPhysicsSubsystem::shutdown() (and its whole-
  // system Jolt teardown) always runs after this, but there's no reason to
  // rely on that alone when removing bodies here is free.
  for (const auto &[coord, data] : mActive)
    if (data.physicsBodyId != 0xFFFFFFFF)
      mState.physicsSystem.removeTerrainChunk(data.physicsBodyId);
  mState.terrain.shutdown();
  mActive.clear();
  mScatterByChunk.clear();
  mInteractiveTrees.clear();
  mCollidableRocks.clear();
}

const TerrainSettings &VkTerrainSubsystem::settings() const {
  return mState.terrain.settings();
}

void VkTerrainSubsystem::regenerate(const TerrainSettings &newSettings) {
  if (!mReady)
    return;

  Registry &reg = mState.scene.registry();

  // Full teardown of every active chunk's render-side state -- more
  // thorough than shutdown() does (that skips ECS entity destruction since
  // the whole scene is going away with it; regenerate() keeps the app
  // running, so stale entities would otherwise linger in the Hierarchy
  // forever). Mirrors the per-chunk unload branch in addFrameInstances(),
  // just applied to everything currently active at once.
  for (const auto &[coord, data] : mActive) {
    if (data.entityId != 0)
      reg.destroy(data.entityId);
    if (data.physicsBodyId != 0xFFFFFFFF)
      mState.physicsSystem.removeTerrainChunk(data.physicsBodyId);
    if (data.meshHandle != UINT32_MAX)
      mState.renderer->destroyMesh(data.meshHandle);
  }
  for (const auto &[coord, trees] : mInteractiveTrees)
    for (uint32_t entityId : trees)
      reg.destroy(entityId);
  for (const auto &[coord, rocks] : mCollidableRocks)
    for (uint32_t entityId : rocks)
      reg.destroy(entityId);

  mActive.clear();
  mScatterByChunk.clear();
  mInteractiveTrees.clear();
  mCollidableRocks.clear();
  mVegDirty = true; // scatter set just emptied; batches must clear too

  mState.terrain.shutdown();
  mState.terrain.init(newSettings, mManifest);
  // mQuery held references into the old noise set/edits grid, both just
  // replaced -- must be rebuilt, not just left pointing at freed state.
  mQuery = std::make_unique<TerrainQuery>(mState.terrain.settings(),
                                          mState.terrain.noiseSet(),
                                          &mState.terrain.editsGrid());
  mState.renderer->params().farPlane = farPlaneForSettings(newSettings);

  // R4: layer meshes are real files now, not seed-derived procedural
  // geometry -- no refresh needed here (unlike the old per-species
  // buildSpeciesMesh(newSettings.seed, ...) reload this replaced).
}

void VkTerrainSubsystem::addFrameInstances() {
  if (!mReady || !mState.renderer)
    return;

  Registry &reg = mState.scene.registry();
  const float chunkWorldSize = mState.terrain.settings().chunkWorldSize;

  for (auto &upload : mState.terrain.takePendingUploads()) {
    if (upload.mesh.submeshes.empty()) {
      std::fprintf(stderr,
                   "[VkTerrainSubsystem] empty mesh for chunk (%d,%d) L%d\n",
                   upload.coord.x, upload.coord.z, upload.lod);
      continue;
    }

    auto activeIt = mActive.find(upload.coord);
    const uint32_t existingHandle =
        activeIt != mActive.end() ? activeIt->second.meshHandle : UINT32_MAX;
    const int existingLod = activeIt != mActive.end() ? activeIt->second.lod : -1;

    uint32_t meshHandle;
    if (existingHandle != UINT32_MAX && existingLod == upload.lod) {
      // Same-LOD re-upload (brush rebuild): refresh geometry in place, the
      // handle and its BLAS slot stay stable.
      if (!mState.renderer->updateMeshFromData(existingHandle, upload.mesh)) {
        std::fprintf(stderr,
                     "[VkTerrainSubsystem] rebuild failed for chunk (%d,%d) L%d\n",
                     upload.coord.x, upload.coord.z, upload.lod);
        continue;
      }
      meshHandle = existingHandle;
    } else {
      char debugName[64];
      std::snprintf(debugName, sizeof(debugName), "TerrainChunk_%d_%d_L%d",
                    upload.coord.x, upload.coord.z, upload.lod);
      meshHandle = mState.renderer->createMeshFromData(upload.mesh, debugName);
      if (meshHandle == UINT32_MAX) {
        std::fprintf(stderr,
                     "[VkTerrainSubsystem] createMeshFromData failed for "
                     "chunk (%d,%d)\n",
                     upload.coord.x, upload.coord.z);
        continue;
      }
      // LOD replacement: evict the old mesh + BLAS (deferred inside the
      // renderer past in-flight frames). Unloaded-then-revisited chunks
      // rebuild from a fresh worker job, so nothing is kept around "just in
      // case" -- GPU memory tracks the ACTIVE set now instead of growing
      // with every (coord, lod) ever visited.
      if (existingHandle != UINT32_MAX)
        mState.renderer->destroyMesh(existingHandle);
    }

    ChunkRenderData &data = mActive[upload.coord];
    data.meshHandle = meshHandle;
    data.lod = upload.lod;
    if (data.entityId == 0) {
      char name[64];
      std::snprintf(name, sizeof(name), "TerrainChunk_%d_%d", upload.coord.x,
                    upload.coord.z);
      const auto entity = mState.scene.createEmptyEntity(name);
      const glm::vec2 center = chunkCenterWorld(upload.coord, chunkWorldSize);
      reg.get<TransformComponent>(entity).position =
          glm::vec3(center.x, 0.0f, center.y);
      auto &mc = reg.emplace<MeshComponent>(entity);
      mc.assetId = name;
      mc.isTerrain = true;
      mc.visible = true;
      data.entityId = entity;
    }

    // An edited chunk with an active Jolt heightfield needs new collision
    // heights (heightfields are immutable). Sampled asynchronously on the
    // worker pool now -- the old synchronous sampleCollisionHeights() here
    // cost tens of milliseconds of main-thread time per chunk. The body is
    // replaced when the sample lands (updateTerrainCollision()'s drain).
    if (data.physicsBodyId != 0xFFFFFFFF)
      mState.terrain.requestCollisionHeights(upload.coord);

    // Vegetation batches only rebuild when some chunk's scatter actually
    // changed (had or gained instances).
    auto scatterIt = mScatterByChunk.find(upload.coord);
    const bool hadScatter =
        scatterIt != mScatterByChunk.end() && !scatterIt->second.empty();
    if (hadScatter || !upload.scatter.empty())
      mVegDirty = true;
    mScatterByChunk[upload.coord] = std::move(upload.scatter);
  }

  for (const ChunkCoord &coord : mState.terrain.takePendingUnloads()) {
    auto it = mActive.find(coord);
    if (it != mActive.end()) {
      if (it->second.entityId != 0)
        reg.destroy(it->second.entityId);
      if (it->second.physicsBodyId != 0xFFFFFFFF)
        mState.physicsSystem.removeTerrainChunk(it->second.physicsBodyId);
      if (it->second.meshHandle != UINT32_MAX)
        mState.renderer->destroyMesh(it->second.meshHandle);
      mActive.erase(it);
    }
    auto scatterIt = mScatterByChunk.find(coord);
    if (scatterIt != mScatterByChunk.end()) {
      if (!scatterIt->second.empty())
        mVegDirty = true;
      mScatterByChunk.erase(scatterIt);
    }
    auto treeIt = mInteractiveTrees.find(coord);
    if (treeIt != mInteractiveTrees.end()) {
      for (uint32_t entityId : treeIt->second)
        reg.destroy(entityId);
      mInteractiveTrees.erase(treeIt);
    }
    auto rockIt = mCollidableRocks.find(coord);
    if (rockIt != mCollidableRocks.end()) {
      for (uint32_t entityId : rockIt->second)
        reg.destroy(entityId);
      mCollidableRocks.erase(rockIt);
    }
  }

  for (const auto &[coord, data] : mActive) {
    const glm::vec2 center = chunkCenterWorld(coord, chunkWorldSize);
    const glm::mat4 transform =
        glm::translate(glm::mat4(1.0f), glm::vec3(center.x, 0.0f, center.y));
    mState.renderer->addInstance(data.meshHandle, transform,
                                 /*isTerrain=*/true);
  }

  // Per-layer scatter batches (GPU-instanced -- see
  // VulkanRenderer::setVegetationBatches()'s doc comment) are only
  // reassembled and re-uploaded when the scatter set changed; the renderer
  // keeps drawing the previous buffers otherwise.
  if (mVegDirty) {
    mVegDirty = false;
    rebuildVegetationBatches();
  }

  promoteInteractiveTrees(mState.renderer->params().camPos);
  promoteCollidableRocks(mState.renderer->params().camPos);
  updateTerrainCollision(mState.renderer->params().camPos);

  // R3: feed the camera's current biome weights to the renderer's tonemap
  // camera-grade (VulkanRenderer owns the exponential smoothing). mQuery is
  // the same TerrainQuery raycastTerrain()/applyHeightBrush() already use.
  if (mQuery) {
    const glm::vec3 camPos = mState.renderer->params().camPos;
    const BiomeWeights w = mQuery->biomeWeightsAt(glm::vec2(camPos.x, camPos.z));
    mState.renderer->setCameraBiomeWeights(glm::vec3(w.meadow, w.forest, w.mountain));
  }
}

void VkTerrainSubsystem::rebuildVegetationBatches() {
  const size_t layerCount = mManifest.layers.size();
  const TerrainSettings &settings = mState.terrain.settings();
  const float chunkWorldSize = settings.chunkWorldSize;

  std::vector<vkrhi::VulkanRenderer::VegBatch> batches(layerCount);
  for (size_t li = 0; li < layerCount; ++li) {
    batches[li].mesh = li < mLayerMesh.size() ? mLayerMesh[li] : UINT32_MAX;

    // Per-batch render behavior comes from the SAME effectiveLayer() the
    // worker threads used for placement, so the draw distance the renderer
    // culls at cannot drift from the one the scatter thinned against.
    const ScatterLayer &layer = mManifest.layers[li];
    const EffectiveScatterLayer eff = effectiveLayer(layer, settings);
    batches[li].drawDistance =
        eff.maxDrawDistance < 1.0e6f ? eff.maxDrawDistance : 0.0f;
    // Grass layers additionally honor the grassCastShadows switch: the
    // manifest's castRayShadow=false is the safe default, and the setting
    // can opt them back IN (the reverse is not offered -- a layer that
    // authored itself out of the TLAS has a reason).
    batches[li].rayTracedShadows =
        layer.castRayShadow ||
        (layer.type == ScatterLayerType::Grass && settings.grassCastShadows);
    batches[li].windStrength = eff.windStrength;
    batches[li].windSpeed = eff.windSpeed;
    batches[li].windMeshHeight =
        li < mLayerMeshHeight.size() ? mLayerMeshHeight[li] : 1.0f;
    batches[li].groundOcclusion = eff.groundOcclusion;
    batches[li].foliageSssStrength = layer.foliageSssStrength;
  }

  // Culling ranges. Trees/rocks get one range per (chunk, layer) -- a few
  // dozen instances visible from far away, where finer granularity would
  // only add per-range test cost. Grass layers set cullCellSize, which
  // subdivides the chunk into a grid of cells and emits one range per
  // occupied cell instead; see ScatterLayer::cullCellSize for why grass
  // cannot work with chunk-sized ranges.
  //
  // Both paths share one code path, with cell index 0 meaning "the whole
  // chunk" for the non-subdivided layers.
  //
  // Instances must be CONTIGUOUS per range (a range is a [first, count) span
  // into the batch's instance buffer), so a subdivided layer's instances are
  // collected and sorted by cell before being appended. This holds one
  // chunk's worth.
  std::vector<std::vector<std::pair<uint32_t, vkrhi::VulkanRenderer::VegInstanceGpu>>>
      pending(layerCount);

  for (const auto &[coord, scatterList] : mScatterByChunk) {
    if (scatterList.empty())
      continue;
    const glm::vec2 chunkOrigin = chunkMinCorner(coord, chunkWorldSize);

    for (auto &p : pending)
      p.clear();

    for (const ScatterInstance &s : scatterList) {
      if (s.layerIndex < 0 || static_cast<size_t>(s.layerIndex) >= layerCount)
        continue;
      const size_t li = static_cast<size_t>(s.layerIndex);
      if (batches[li].mesh == UINT32_MAX)
        continue;

      const glm::vec3 pos = glm::vec3(s.transform[3]);
      uint32_t cell = 0;
      const float cellSize = mManifest.layers[li].cullCellSize;
      if (cellSize > 0.01f) {
        const int cells =
            std::max(1, static_cast<int>(std::ceil(chunkWorldSize / cellSize)));
        const int cx = glm::clamp(
            static_cast<int>((pos.x - chunkOrigin.x) / cellSize), 0, cells - 1);
        const int cz = glm::clamp(
            static_cast<int>((pos.z - chunkOrigin.y) / cellSize), 0, cells - 1);
        cell = static_cast<uint32_t>(cz * cells + cx);
      }

      vkrhi::VulkanRenderer::VegInstanceGpu gpu;
      gpu.model = s.transform;
      gpu.colorJitter = glm::vec4(s.colorJitter, 0.0f);
      gpu.biomeWeights = glm::vec4(s.biomeWeights, 0.0f);
      pending[li].emplace_back(cell, gpu);
    }

    // Append each layer's instances grouped by cell, recording one range per
    // cell. Sorting by cell is what makes each range contiguous.
    for (size_t li = 0; li < layerCount; ++li) {
      if (pending[li].empty())
        continue;
      auto &items = pending[li];
      std::stable_sort(items.begin(), items.end(),
                       [](const auto &a, const auto &b) { return a.first < b.first; });

      const float meshRadius =
          li < mLayerBoundsRadius.size() ? mLayerBoundsRadius[li] : 1.0f;

      size_t i = 0;
      while (i < items.size()) {
        const uint32_t cell = items[i].first;
        const uint32_t first =
            static_cast<uint32_t>(batches[li].instances.size());
        glm::vec3 bMin(std::numeric_limits<float>::max());
        glm::vec3 bMax(std::numeric_limits<float>::lowest());
        uint32_t count = 0;
        for (; i < items.size() && items[i].first == cell; ++i) {
          const auto &gpu = items[i].second;
          const glm::vec3 pos = glm::vec3(gpu.model[3]);
          // Radius must account for the NON-UNIFORM scale introduced by
          // heightScale: column 0 carries the horizontal scale, column 1 the
          // (larger) vertical one. Taking the max keeps the bounds
          // conservative -- using column 0 alone would under-size the AABB of
          // a stretched tree and pop it out of view early.
          const float sxz = glm::length(glm::vec3(gpu.model[0]));
          const float sy = glm::length(glm::vec3(gpu.model[1]));
          const float radius = meshRadius * std::max(sxz, sy);
          bMin = glm::min(bMin, pos - glm::vec3(radius));
          bMax = glm::max(bMax, pos + glm::vec3(radius));
          batches[li].instances.push_back(gpu);
          ++count;
        }
        vkrhi::VulkanRenderer::VegRange range;
        range.first = first;
        range.count = count;
        range.boundsMin = bMin;
        range.boundsMax = bMax;
        batches[li].ranges.push_back(range);
      }
    }
  }

  if (std::getenv("GLGEN_VK_SCATTER_STATS")) {
    for (size_t li = 0; li < layerCount; ++li)
      std::fprintf(stderr,
                   "[scatter] %-14s instances=%-7zu ranges=%-6zu drawDist=%.0f "
                   "rtShadow=%d wind=%.2f meshH=%.2f\n",
                   mManifest.layers[li].name.c_str(), batches[li].instances.size(),
                   batches[li].ranges.size(), batches[li].drawDistance,
                   batches[li].rayTracedShadows ? 1 : 0, batches[li].windStrength,
                   batches[li].windMeshHeight);
    std::fprintf(stderr, "[scatter] chunks with scatter: %zu\n", mScatterByChunk.size());
  }

  std::vector<vkrhi::VulkanRenderer::VegBatch> nonEmpty;
  for (auto &batch : batches)
    if (batch.mesh != UINT32_MAX && !batch.instances.empty())
      nonEmpty.push_back(std::move(batch));
  mState.renderer->setVegetationBatches(nonEmpty);
}

void VkTerrainSubsystem::updateTerrainCollision(glm::vec3 cameraWorldPos) {
  const float chunkWorldSize = mState.terrain.settings().chunkWorldSize;
  const int radius = mState.terrain.settings().collisionChunkRadius;
  const uint32_t chunkResolution = mState.terrain.settings().chunkResolution;
  const ChunkCoord cameraChunk = chunkCoordFromWorldXZ(
      glm::vec2(cameraWorldPos.x, cameraWorldPos.z), chunkWorldSize);

  auto chebyshevDist = [&](ChunkCoord c) {
    const int dx = std::abs(c.x - cameraChunk.x);
    const int dz = std::abs(c.z - cameraChunk.z);
    return dx > dz ? dx : dz;
  };

  int budget = static_cast<int>(mState.terrain.settings().collisionUpdatesPerFrame);

  // Demote: active chunks with a body that are now outside the radius.
  for (auto &[coord, data] : mActive) {
    if (budget <= 0)
      break;
    if (data.physicsBodyId == 0xFFFFFFFF || chebyshevDist(coord) <= radius)
      continue;
    mState.physicsSystem.removeTerrainChunk(data.physicsBodyId);
    data.physicsBodyId = 0xFFFFFFFF;
    --budget;
  }

  // Promote: REQUEST collision heights for bodyless chunks in radius. The
  // sampling (a full chunkResolution^2 noise grid, tens of milliseconds)
  // runs on the worker pool now -- doing it synchronously here stalled the
  // main thread by up to ~2 chunks' worth EVERY frame near chunk
  // boundaries. Collision always samples at chunkResolution (LOD0),
  // independent of the chunk's current visual LOD.
  for (auto &[coord, data] : mActive) {
    if (budget <= 0)
      break;
    if (data.physicsBodyId != 0xFFFFFFFF || chebyshevDist(coord) > radius)
      continue;
    if (mState.terrain.requestCollisionHeights(coord))
      --budget;
  }

  // Drain completed samples into Jolt bodies. Covers both fresh promotions
  // and brush-refresh requests (existing body: replace, heightfields are
  // immutable). Chunks that unloaded or left the radius while their sample
  // was in flight are dropped; the demote pass above already handles any
  // body they still hold.
  for (auto &result : mState.terrain.takeCompletedCollision()) {
    auto it = mActive.find(result.coord);
    if (it == mActive.end() || chebyshevDist(result.coord) > radius ||
        result.heights.empty())
      continue;
    if (it->second.physicsBodyId != 0xFFFFFFFF)
      mState.physicsSystem.removeTerrainChunk(it->second.physicsBodyId);
    const glm::vec2 minCorner = chunkMinCorner(result.coord, chunkWorldSize);
    it->second.physicsBodyId = mState.physicsSystem.addTerrainChunk(
        result.heights, chunkResolution, minCorner, chunkWorldSize);
  }
}

void VkTerrainSubsystem::promoteInteractiveTrees(glm::vec3 cameraWorldPos) {
  const float chunkWorldSize = mState.terrain.settings().chunkWorldSize;
  const int radius = mState.terrain.settings().interactiveTreeChunkRadius;
  const ChunkCoord cameraChunk = chunkCoordFromWorldXZ(
      glm::vec2(cameraWorldPos.x, cameraWorldPos.z), chunkWorldSize);
  Registry &reg = mState.scene.registry();

  auto chebyshevDist = [&](ChunkCoord c) {
    const int dx = std::abs(c.x - cameraChunk.x);
    const int dz = std::abs(c.z - cameraChunk.z);
    return dx > dz ? dx : dz;
  };

  // Demote: chunks currently promoted but now unloaded or out of range.
  for (auto it = mInteractiveTrees.begin(); it != mInteractiveTrees.end();) {
    const bool stillActive = mScatterByChunk.find(it->first) != mScatterByChunk.end();
    if (!stillActive || chebyshevDist(it->first) > radius) {
      for (uint32_t entityId : it->second)
        reg.destroy(entityId);
      it = mInteractiveTrees.erase(it);
    } else {
      ++it;
    }
  }

  // Promote: active chunks within range not yet processed this "visit".
  for (const auto &[coord, scatterList] : mScatterByChunk) {
    if (mInteractiveTrees.find(coord) != mInteractiveTrees.end())
      continue;
    if (chebyshevDist(coord) > radius)
      continue;

    std::vector<uint32_t> promoted;
    for (size_t i = 0; i < scatterList.size(); ++i) {
      const ScatterInstance &s = scatterList[i];
      // ~20% of eligible (layer.interactive == true) instances, capped
      // implicitly by this stride rather than an explicit per-chunk count.
      if (!s.interactive || (i % 5 != 0))
        continue;
      const glm::vec3 pos = glm::vec3(s.transform[3]);
      const auto entity = mState.scene.createEmptyEntity("InteractiveTree");
      reg.get<TransformComponent>(entity).position = pos;
      auto &tree = reg.emplace<TreeComponent>(entity);
      tree.chunkX = coord.x;
      tree.chunkZ = coord.z;
      auto &rb = reg.emplace<RigidbodyComponent>(entity);
      rb.type = RigidbodyComponent::Type::Static;
      auto &col = reg.emplace<ColliderComponent>(entity);
      col.shape = ColliderComponent::Shape::Capsule;
      col.dimensions = glm::vec3(0.22f, 1.8f, 0.0f); // x=radius y=height
      promoted.push_back(entity);
    }
    // Recorded even if empty, so an in-range chunk with no eligible trees
    // isn't rescanned every frame.
    mInteractiveTrees.emplace(coord, std::move(promoted));
  }
}

void VkTerrainSubsystem::promoteCollidableRocks(glm::vec3 cameraWorldPos) {
  // R4 (plan §6d): rocks get static colliders within collisionChunkRadius,
  // the same radius terrain heightfields use (rocks are static geometry --
  // matching their collision budget to terrain's makes sense, rather than
  // interactive trees' separate, larger interactiveTreeChunkRadius).
  const float chunkWorldSize = mState.terrain.settings().chunkWorldSize;
  const int radius = mState.terrain.settings().collisionChunkRadius;
  const ChunkCoord cameraChunk = chunkCoordFromWorldXZ(
      glm::vec2(cameraWorldPos.x, cameraWorldPos.z), chunkWorldSize);
  Registry &reg = mState.scene.registry();

  auto chebyshevDist = [&](ChunkCoord c) {
    const int dx = std::abs(c.x - cameraChunk.x);
    const int dz = std::abs(c.z - cameraChunk.z);
    return dx > dz ? dx : dz;
  };

  for (auto it = mCollidableRocks.begin(); it != mCollidableRocks.end();) {
    const bool stillActive = mScatterByChunk.find(it->first) != mScatterByChunk.end();
    if (!stillActive || chebyshevDist(it->first) > radius) {
      for (uint32_t entityId : it->second)
        reg.destroy(entityId);
      it = mCollidableRocks.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto &[coord, scatterList] : mScatterByChunk) {
    if (mCollidableRocks.find(coord) != mCollidableRocks.end())
      continue;
    if (chebyshevDist(coord) > radius)
      continue;

    std::vector<uint32_t> promoted;
    for (const ScatterInstance &s : scatterList) {
      if (s.layerIndex < 0 || static_cast<size_t>(s.layerIndex) >= mManifest.layers.size())
        continue;
      const ScatterLayer &layer = mManifest.layers[static_cast<size_t>(s.layerIndex)];
      if (layer.collision != ScatterCollisionType::ConvexOrSphere)
        continue;

      const glm::vec3 pos = glm::vec3(s.transform[3]);
      // Uniform scale baked into the transform's basis columns (placement
      // always applies rotation then uniform scale -- see
      // TerrainScatter.cpp's makeInstance()) -- recover it as the length of
      // any basis column to size the collider to this specific instance.
      const float scale = glm::length(glm::vec3(s.transform[0]));
      const auto entity = mState.scene.createEmptyEntity("CollidableRock");
      reg.get<TransformComponent>(entity).position = pos;
      auto &rb = reg.emplace<RigidbodyComponent>(entity);
      rb.type = RigidbodyComponent::Type::Static;
      auto &col = reg.emplace<ColliderComponent>(entity);
      col.shape = ColliderComponent::Shape::Sphere;
      // The layer's own unscaled mesh-bounds radius (computed once in
      // loadLayerMeshes()) times this instance's placement scale, so
      // colliders actually match the asset instead of a generic guess.
      const float baseRadius = mLayerBoundsRadius[static_cast<size_t>(s.layerIndex)];
      col.dimensions = glm::vec3(baseRadius * scale, 0.0f, 0.0f);
      promoted.push_back(entity);
    }
    mCollidableRocks.emplace(coord, std::move(promoted));
  }
}

TerrainQuery::RaycastHit VkTerrainSubsystem::raycastTerrain(
    glm::vec3 origin, glm::vec3 dir, float maxDistance) const {
  if (!mQuery)
    return {};
  return mQuery->raycast(origin, dir, maxDistance);
}

void VkTerrainSubsystem::applyHeightBrush(glm::vec2 worldXZ, float radius,
                                          float strength, bool lower) {
  if (!mReady)
    return;
  mState.terrain.applyHeightBrush(worldXZ, radius, strength, lower);
}

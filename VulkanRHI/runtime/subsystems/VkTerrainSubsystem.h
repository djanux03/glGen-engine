#pragma once

#include "IEngineSubsystem.h"
#include "ScatterManifest.h"
#include "TerrainChunkManager.h"
#include "TerrainQuery.h"
#include "TerrainIslands.h"
#include "TerrainTypes.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

struct VkAppState;

// Phase 2 of TERRAIN_GENERATOR_PLAN.md: renderer-side glue for
// TerrainChunkManager's chunk streaming. The subsystem itself stays
// init/shutdown-only (mirrors VkPhysicsSubsystem) -- per-frame streaming is
// driven directly by VkCoreAppLayer::update() calling
// `state.terrain.streamUpdate(...)`, same as `state.physicsSystem.update()`.
// This class's job is purely to turn TerrainChunkManager's plain-data
// results into GPU meshes + ECS entities + renderer instances each frame.
//
// Rendering bypasses AssetManager's OBJ registry, continuing Phase 1's
// direct-call pattern (see TERRAIN_GENERATOR_PLAN.md's Phase 1 notes; the
// `registerRuntimeOBJRaw` that originally forced this has since been replaced
// by AssetManager::registerMeshData(), which does keep the CPU MeshData --
// routing chunks through it is possible now, just not yet done): chunk meshes
// are created via
// VulkanRenderer::createMeshFromData()/addInstance() directly, and
// VulkanRenderer::growScene() (not finalizeScene()) is used to add their
// BLAS incrementally without a full-scene stall. ECS entities are still
// created per active chunk purely for Hierarchy/Inspector visibility --
// otherwise inert for rendering, same as Phase 1.
//
// Phase 3 adds vegetation (R4 of MEADOW_TERRAIN_REVAMP_PLAN.md replaces the
// fixed 9-species procedural set with a data-driven ScatterManifest): one
// real mesh per manifest layer, loaded via AssetManager (loadOBJ/loadFBX/
// loadGLTF depending on extension) + recentered, registered once at
// initialize() (same createMeshFromData()+growScene() path as chunk
// meshes), rendered via VulkanRenderer::setVegetationBatches() (GPU
// instancing -- see that API's doc comment) rather than one ECS
// entity/addInstance() per plant. A small subset of interactive-eligible
// tree instances near the camera get "promoted" to physically-collidable
// marker entities -- TransformComponent + TreeComponent +
// RigidbodyComponent + ColliderComponent(Capsule), deliberately no
// MeshComponent, since the tree keeps rendering through its layer's batch
// and doesn't need its own draw. R4 adds the same promotion for rock
// layers (ColliderComponent(Sphere), no TreeComponent).
//
// Phase 4 adds terrain collision: collisionChunkRadius (default 2) is
// smaller than viewDistanceChunks (default 6), so most active/rendered
// chunks have no collision by design, and eligibility changes as the camera
// moves *within* the view radius -- independent of when a chunk's mesh was
// uploaded. So collision is re-evaluated every frame against the current
// camera distance to every active chunk, mirroring
// promoteInteractiveTrees()'s demote-then-promote shape, rather than being
// tied to TerrainChunkManager's upload/unload events.
//
// Phase 5 adds height-brush editing: owns a TerrainQuery (referencing
// mState.terrain's settings/noise set/edits grid) for raycasting, and
// forwards applyHeightBrush() to TerrainChunkManager. A brush-triggered
// rebuild for an already-cached (coord,lod) mesh handle updates it in place
// (VulkanRenderer::updateMeshFromData()) instead of being silently
// swallowed by the mesh cache; an already-collision-active chunk that
// rebuilds gets its Jolt body replaced too (heightfields are immutable).
//
// regenerate() (the Terrain Generator editor panel's "Regenerate All
// Chunks" button) is a full teardown + rebuild with new TerrainSettings:
// tears down every active chunk's render-side state exactly like
// shutdown() does, resets TerrainChunkManager (which wipes brush edits --
// see HeightOffsetGrid::clear()), rebuilds mQuery (it referenced the old
// noise set/edits grid, both replaced), and refreshes the 9 vegetation
// species meshes in place since their variant seed derives from
// TerrainSettings::seed. Safe to call from editor UI code: this frame's
// streamUpdate()/addFrameInstances() already completed by the time
// editor.draw() runs (main.cpp's frame loop is strictly sequential).
class VkTerrainSubsystem final : public IEngineSubsystem {
public:
  explicit VkTerrainSubsystem(VkAppState &state) : mState(state) {}

  std::string name() const override { return "VkTerrainSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override {
    return {"VkPhysicsSubsystem"};
  }

  bool initialize() override;
  void shutdown() override;

  // --- terrain lifetime ---------------------------------------------------
  // The engine boots with NO terrain: an empty scene is the honest starting
  // point for an editor, and terrain is content you create rather than
  // something the runtime assumes. initialize() only stages settings and
  // loads the scatter manifest; these bring terrain in and out.
  //
  // (GLGEN_TERRAIN_ON_START=1 creates it at startup instead, for headless
  // runs that need ground without a UI to click.)
  bool create(const TerrainSettings &settings);
  void destroy();
  bool hasTerrain() const { return mReady; }

  // Settings the next create() will use, and what the Terrain panel edits
  // before any terrain exists. Once terrain is live, settings() is the
  // authority.
  const TerrainSettings &pendingSettings() const { return mPendingSettings; }

  // Called once per frame from main.cpp, AFTER VulkanRenderSystem::update()
  // (which clears+rebuilds mInstances from the ECS) so streamed terrain
  // instances survive into this frame's draw. Drains
  // TerrainChunkManager::takePendingUploads()/takePendingUnloads(), uploads
  // or tears down the corresponding render-side state, re-adds an instance
  // for every currently active chunk, rebuilds this frame's per-species
  // vegetation batches, and promotes/demotes interactive trees by distance
  // to the camera.
  void addFrameInstances();

  // Brush-tool API (called by the editor). Safe to call even if a raycast
  // hasn't been done yet this frame -- raycastTerrain() is stateless.
  TerrainQuery::RaycastHit raycastTerrain(glm::vec3 origin, glm::vec3 dir,
                                          float maxDistance = 500.0f) const;
  void applyHeightBrush(glm::vec2 worldXZ, float radius, float strength,
                        bool lower);

  // Terrain Generator panel API.
  const TerrainSettings &settings() const;
  const ScatterManifest &manifest() const { return mManifest; }
  ScatterManifest &manifest() { return mManifest; }
  void regenerate(const TerrainSettings &newSettings);

  // Procedural terrain height at a world XZ position (0 if the terrain
  // hasn't been initialized yet) -- used by main.cpp to place the spawn
  // camera above the actual generated surface instead of a hardcoded Y.
  float heightAt(glm::vec2 worldXZ) const {
    return mQuery ? mQuery->heightAt(worldXZ) : 0.0f;
  }

private:
  struct ChunkRenderData {
    uint32_t meshHandle = 0xFFFFFFFF; // VulkanRenderer::MeshHandle; sentinel = none
    int lod = -1;                     // LOD the current mesh was built at
    uint32_t entityId = 0;   // Registry::EntityId; 0 == none (ids start at 1)
    uint32_t physicsBodyId = 0xFFFFFFFF; // Jolt body id; sentinel = none
  };

  // Destroys every active chunk's mesh, physics body and marker entities.
  // Shared by destroy() and regenerate(), which need exactly the same
  // teardown for different reasons.
  void releaseAllChunks();

  // Pushes the terrain's water into the renderer: the resolved sea level, and
  // a sampled grid of TerrainWater::waterSurfaceAt() covering the streamed
  // region so lakes render at their own altitudes. Rebuilt when the camera
  // leaves the area the current grid covers -- the terrain stays the single
  // authority for where water is, exactly as it is for where the ground is.
  void syncWaterToRenderer();

public:
  // Rasterises a top-down RGBA map of the whole world: ocean shaded by depth,
  // land tinted by its island's archetype and shaded by altitude, beaches
  // picked out. Island layout is a GLOBAL property and no in-engine camera can
  // see it, which is why this exists at all. Shared by terrain.world_map()
  // (writes a PNG) and the editor's World Map panel (uploads a texture).
  void buildWorldMapRGBA(std::vector<unsigned char> &outRgba, int pixels,
                         float extent) const;

  // Which landmass covers a world XZ, and what kind of country it is. Never
  // null: open ocean returns a neutral island.
  const IslandInfo &islandAt(glm::vec2 worldXZ) const;

private:
  glm::vec2 mWaterFieldCentre{0.0f};
  bool mWaterFieldValid = false;
  std::vector<float> mWaterFieldScratch;

  void promoteInteractiveTrees(glm::vec3 cameraWorldPos);
  void promoteCollidableRocks(glm::vec3 cameraWorldPos);
  void updateTerrainCollision(glm::vec3 cameraWorldPos);

  // Loads (or reloads) every manifest layer's mesh via AssetManager +
  // createMeshFromData(), filling mLayerMesh 1:1 with mManifest.layers.
  // Shared by initialize() and regenerate().
  void loadLayerMeshes();

  VkAppState &mState;
  // True only while terrain actually exists. Everything per-frame is gated
  // on it, so "no terrain" costs nothing rather than streaming an empty world.
  bool mReady = false;
  // What create() will use. Editable through the Terrain panel before any
  // terrain exists, so settings survive create/destroy cycles.
  TerrainSettings mPendingSettings;

  // Constructed in initialize() once mState.terrain is set up (references
  // its settings/noise set/edits grid, all stable for its lifetime).
  std::unique_ptr<TerrainQuery> mQuery;

  std::unordered_map<ChunkCoord, ChunkRenderData> mActive;

  // Rebuild-vegetation flag: per-layer GPU batches (and their TLAS inputs)
  // are only reassembled when the scatter set actually changed -- a chunk
  // with vegetation streamed in or out -- instead of every frame.
  bool mVegDirty = true;

  // Builds per-layer VegBatch lists (instances + per-chunk cull ranges)
  // from mScatterByChunk and hands them to the renderer. Called from
  // addFrameInstances() when mVegDirty.
  void rebuildVegetationBatches();

  // R4: the scatter manifest driving placement (Engine/Terrain/
  // TerrainChunkManager owns a copy for worker-thread placement; this one
  // is what the render side reads to resolve layerIndex -> mesh/behavior).
  ScatterManifest mManifest;
  // One mesh per manifest layer, in the same order as mManifest.layers,
  // built/refreshed by loadLayerMeshes(). UINT32_MAX = failed to load.
  std::vector<uint32_t> mLayerMesh;
  // Bounding-sphere-ish radius of each layer's UNSCALED mesh (half the
  // diagonal of its AABB), for sizing rock colliders to the actual asset
  // instead of a generic guess -- multiply by an instance's own placement
  // scale (see promoteCollidableRocks()).
  std::vector<float> mLayerBoundsRadius;
  // Y extent of each layer's UNSCALED mesh, in mesh units. Fed to the wind
  // shader so it can normalize a vertex's height into a 0..1 fraction of the
  // plant's own height (see VegBatch::windMeshHeight).
  std::vector<float> mLayerMeshHeight;

  // Scatter placements for every currently-active chunk (mirrors mActive's
  // lifetime: populated on upload, erased on unload).
  std::unordered_map<ChunkCoord, std::vector<ScatterInstance>> mScatterByChunk;

  // Interactive-tree marker entities, keyed by the chunk they were promoted
  // from -- demoted (destroyed) when that chunk falls outside
  // interactiveTreeChunkRadius or unloads.
  std::unordered_map<ChunkCoord, std::vector<uint32_t>> mInteractiveTrees;

  // R4: static rock collider marker entities, keyed by chunk -- promoted/
  // demoted by distance exactly like mInteractiveTrees (destroyed on
  // demote, recreated on promote), gated by ScatterLayer::collision ==
  // ConvexOrSphere instead of tree-eligibility.
  std::unordered_map<ChunkCoord, std::vector<uint32_t>> mCollidableRocks;
};

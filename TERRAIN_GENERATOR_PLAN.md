# Comprehensive Vulkan Terrain Generator — Design Plan

## Context

glGenVk's current terrain (`VulkanRHI/VulkanRenderer.{h,cpp}`, `shaders/vulkan/terrain.{task,mesh,frag}`) is a fixed 24×24-unit GPU mesh-shader patch: fully procedural, no streaming, no LOD, no collision, no edits. It exists mainly as a tech demo of `VK_EXT_mesh_shader`.

The old OpenGL engine had a far more complete `Engine/Terrain/TerrainSystem` (now deleted from disk, readable via `git show HEAD:<path>`): chunked async streaming, a 6-biome noise pipeline, procedurally-built vegetation with an interactive-tree subset, a brush-editing tool, and rich material blending. The user wants that level of capability rebuilt for Vulkan, done as a real design exercise up front — this plan is that design, **not an execution order**. Nothing gets written yet.

Two pieces of scaffolding already exist in the current codebase specifically anticipating this work, unused so far:
- `AssetManager::registerRuntimeOBJRaw`/`registerRuntimeOBJ<T>` (`Engine/Assets/AssetManager.h:99-110`) — doc comment literally says "terrain chunks, destruction shards."
- `PhysicsSystem::addTerrainChunk`/`removeTerrainChunk` (`Engine/ECS/Systems/PhysicsSystem.h:42-53`, `PhysicsTerrainChunk.cpp`) — Jolt `HeightFieldShapeSettings`, fully functional, zero callers.

Both are CPU-chunk-shaped, which drives the core architecture decision below.

## Core Architecture Decision

**Build CPU-authoritative chunked terrain, rendered as ordinary indexed meshes through the existing mesh pipeline — not an extension of the GPU mesh-shader approach.**

Why: the mesh-shader terrain has no CPU-readable height data (it's computed transiently per-vertex on the GPU), which blocks collision (Jolt needs a `float` array), brush edits (two sources of truth), and vegetation placement (inherently CPU/ECS). A CPU-chunk model lets one `sampleHeightGrid()` call feed the visual mesh, the Jolt heightfield, and vegetation placement simultaneously — single source of truth, no GPU readback stalls, and it's exactly the shape `registerRuntimeOBJRaw`/`addTerrainChunk` were built for. `terrain.task/.mesh/.frag`, `createTerrainPipeline()`, `kTerrainPatches`, and the terrain fields on `VulkanRenderer::Params` become dead code, deleted once the new system is demoable (end of Phase 1).

`MeshComponent::isTerrain` and `InstancedMeshComponent::useTerrainShading` (both currently-unused bools already in `Engine/ECS/Components.h`) get their first real purpose: routing a chunk mesh to the terrain shading variant and letting vegetation instances sample terrain-relative tint.

## 1. Noise / Height / Biome Pipeline

New `Engine/Terrain/TerrainNoise.h/.cpp`, built on the **existing, unmodified** `Engine/Core/PerlinNoise.h` (still on disk — classic seeded-permutation Perlin with `noise`/`fbm`/`ridgeNoise`, stateless per call after construction ⇒ safe to share read-only across worker threads).

```cpp
struct TerrainNoiseSet {
  PerlinNoise base, warp, temperature, moisture, erosion, detail;
  explicit TerrainNoiseSet(uint32_t seed); // seed + N*1009 per instance (prime offset, avoids axis-correlation)
};

struct TerrainMacroSample { float continentalness, temperature, moisture, mountainMask, broadShape, ridgeShape; };

TerrainMacroSample sampleMacro(const TerrainNoiseSet&, glm::vec2 worldXZ, const TerrainSettings&);
float computeHeight(const TerrainMacroSample&, const TerrainNoiseSet&, glm::vec2 worldXZ, const TerrainSettings&, const HeightOffsetGrid* edits);
BiomeType classifyBiome(const TerrainMacroSample&, const TerrainSettings&);

void sampleHeightGrid(const TerrainNoiseSet&, const TerrainSettings&, glm::vec2 chunkOrigin,
                       float chunkWorldSize, uint32_t samplesPerEdge, const HeightOffsetGrid* edits,
                       std::vector<float>& outHeights, std::vector<BiomeType>* outBiomes);
```

`computeHeight` is the **single height authority** for the whole system — mesh generation, collision, vegetation ground-clamping, and brush raycasting all resolve through it (or the batched `sampleHeightGrid`). This directly avoids the old codebase's biggest recurring flaw: height/raycast logic re-implemented slightly differently in 3+ places.

Six biomes (Ocean/Plains/Forest/Desert/Mountains/Tundra), classified from continentalness/temperature/moisture/mountainMask thresholds — ported concept from the old `classifyLandscapeBiome`. `singleBiomeOnly` defaults **true** (matches the old system's own default) — biome classification and the shading hook are built from day one, but the multi-biome shader blend stays behind a flag, off by default, to keep Phases 1–5 shader work simple. No new noise family (simplex/Worley) in v1; flagged as a Phase 6 stretch for rock-cluster jitter only.

## 2. Chunk Data Model & Streaming

New module `Engine/Terrain/` (engine-core, no Vulkan includes):

```
Engine/Terrain/TerrainTypes.h            // ChunkCoord, ChunkState, BiomeType, VegSpecies
Engine/Terrain/TerrainSettings.h         // TerrainSettings, TerrainMaterialSettings, TerrainBrushSettings
Engine/Terrain/TerrainNoise.h/.cpp       // §1
Engine/Terrain/HeightOffsetGrid.h/.cpp   // §5, brush height edits
Engine/Terrain/TerrainChunkMesher.h/.cpp // heights+biomes -> MeshData, skirts, per-LOD downsample
Engine/Terrain/VegetationPrimitives.h/.cpp // procedural species mesh builders -> MeshData
Engine/Terrain/TerrainVegetation.h/.cpp  // scatterVegetation, PaintedInstanceCache
Engine/Terrain/TerrainQuery.h/.cpp       // heightAt/biomeAt/normalAt/slopeAt/isUnderwater/isChunkLoadedAt/raycast
Engine/Terrain/TerrainChunkManager.h/.cpp // worker pool, chunk table, streamUpdate(), applyBrush()
```

`TerrainChunkManager` produces `MeshData` (CPU) and drives GPU upload through the existing `ModelGpuBackend` indirection, and calls `PhysicsSystem::addTerrainChunk`/`removeTerrainChunk` directly (engine-core, not renderer-specific) — mirrors how `AssetManager` already splits GL-free CPU logic from renderer-registered GPU hooks.

```cpp
struct TerrainSettings {
  uint32_t seed = 1337;
  float chunkWorldSize = 64.0f;
  uint32_t chunkResolution = 33;      // samples/edge at LOD0
  float heightScale = 24.0f, noiseFrequency = 0.01f;
  int octaves = 5; float lacunarity = 2.0f, gain = 0.5f;
  float macroStrength = 1.0f, mountainSpan = 0.35f;
  bool singleBiomeOnly = true;
  float biomeScale = 0.004f, seaLevel = -2.0f;
  int viewDistanceChunks = 6;                  // Chebyshev radius
  uint32_t workerThreads = 4, maxConcurrentJobs = 10;
  uint32_t maxChunkLoadsPerUpdate = 8;          // jobs dispatched per streamUpdate
  uint32_t maxCompletedChunksPerFrame = 4;      // jobs applied (upload+spawn) per frame
  uint32_t maxGpuUploadBytesPerFrame = 8 * 1024 * 1024;
  uint32_t collisionChunkRadius = 2, collisionUpdatesPerFrame = 2;
  bool spawnVegetation = true;
};

enum class ChunkState { Unloaded, Queued, Building, ReadyToUpload, Active, Unloading };
struct TerrainChunk {
  ChunkCoord coord; ChunkState state = ChunkState::Unloaded; int lod = 0;
  EntityId meshEntity = kInvalidEntity;
  std::vector<EntityId> vegetationEntities, interactiveTreeEntities;
  uint32_t physicsBodyId = 0xFFFFFFFF;
  OBJHandle meshAssetHandle;
  uint64_t revision = 0;   // bumped on brush edit -> triggers rebuild
};
```

**`VkTerrainSubsystem`** (`VulkanRHI/runtime/subsystems/VkTerrainSubsystem.h/.cpp`), a thin shim cloning `VkPhysicsSubsystem`'s pattern exactly against the confirmed `IEngineSubsystem` interface (`name/phase/dependencies/initialize/shutdown`, no `update()` — verified in `Engine/Core/IEngineSubsystem.h:31-41`):

```cpp
class VkTerrainSubsystem final : public IEngineSubsystem {
public:
  explicit VkTerrainSubsystem(VkAppState &state) : mState(state) {}
  std::string name() const override { return "VkTerrainSubsystem"; }
  SubsystemPhase phase() const override { return SubsystemPhase::Runtime; }
  std::vector<std::string> dependencies() const override { return {"VkPhysicsSubsystem"}; }
  bool initialize() override;  // seed TerrainNoiseSet, register GPU backend hooks, queue initial chunks
  void shutdown() override;    // drain workers, remove physics bodies, release OBJ handles
private:
  VkAppState &mState;
};
```

Registered in `main.cpp` right after `VkPhysicsSubsystem` (currently line 335: `VkWindowSubsystem → VkPhysicsSubsystem → VkScriptSubsystem → ...`, confirmed), before `VkScriptSubsystem` so Lua scripts can query terrain on their first tick.

Since `IEngineSubsystem` has no per-frame hook, `state.terrain.streamUpdate(...)` gets called directly from `VkCoreAppLayer::update()` next to the confirmed `state.physicsSystem.update(state.scene.registry(), dt)` call at `VulkanRHI/runtime/subsystems/VkCoreAppLayer.cpp:10` — same precedent already established for physics. `VkAppState.h` (confirmed current contents) gains one member, `Terrain::TerrainChunkManager terrain;`, next to `physicsSystem`, plus its stale line-8 comment ("the CPU TerrainSystem are intentionally not represented here yet") gets updated.

**Streaming loop** (`streamUpdate`, once per frame):
1. If camera hasn't crossed a chunk boundary and no chunk is dirty, skip to step 4 (cheap early-out, ported from the old system's "only reevaluate on boundary-cross").
2. On boundary-cross/dirty: recompute desired chunk set (Chebyshev disc, nearest-first), diff vs. tracked chunks, enqueue up to `maxChunkLoadsPerUpdate` build jobs to a bounded worker pool, mark out-of-range chunks for unload.
3. Drain completed jobs: up to `maxCompletedChunksPerFrame`, each gated by `maxGpuUploadBytesPerFrame` — upload via `registerRuntimeOBJRaw`, spawn/update ECS mesh entity, spawn vegetation, queue collision registration.
4. Process collision queue, capped at `collisionUpdatesPerFrame`.
5. Process unload queue (defensive cap ~8/frame): `releaseOBJ`, `removeTerrainChunk`, destroy ECS entities.

**Worker pool**: fixed-size thread pool (`workerThreads`) owned by `TerrainChunkManager`; each job takes `ChunkCoord` + copied `TerrainSettings` + shared-const `TerrainNoiseSet&`, produces `TerrainChunkResult{MeshData, collisionHeights, vector<VegetationInstance>, dominantBiome}` — **no AssetManager/ECS/PhysicsSystem calls inside a job**, enforced by the result type containing only plain data. All engine-object mutation happens back on the main thread in step 3.

**LOD**: 5 levels, `resolution = chunkResolution >> lod` (33→17→9→5→3), selected by Chebyshev chunk-distance. Unlike the old GL texture-array-paging approach, the CPU model rebuilds real vertex positions per LOD (cheap off-thread). Skirts (fixed-depth drop on outer-ring vertices) hide LOD/neighbor seams — no cross-chunk vertex sharing needed. Biome is encoded in `MeshVertex::uv.y` (fits the existing `MeshVertex{pos,uv,normal}` layout unchanged); `uv.x` carries repeating detail-texture UV.

## 3. Vegetation Scattering

`Engine/Terrain/TerrainVegetation.h/.cpp`, invoked **inside the same chunk-build worker job** — reuses the exact height/biome samples already computed for the mesh, no duplicate noise evaluation.

```cpp
enum class VegSpecies { Pine, Oak, Birch, DeadTree, Cactus, Rock, Grass, Bush, Flower };
struct VegetationInstance { glm::mat4 transform; VegSpecies species; bool interactive; };

void scatterVegetation(BiomeType dominant, const TerrainSettings&, glm::vec2 chunkOrigin,
                        float chunkWorldSize, uint32_t chunkSeed, std::vector<VegetationInstance>& out);
```

Placement rules ported 1:1 in concept from the old system: Forest 5m grid gated by tree-density noise (species chosen by threshold: Pine/Oak/Birch), Desert 12m cactus/grass/flower split, Tundra 14m dead-tree-only, Plains 6m grass+flower+bush, Rocks as **cluster** placement (anchor + 3–6 satellites in a ring, not a uniform grid) for natural boulder fields. Deterministic per chunk via `chunkSeed = hash(seed, coord.x, coord.z)` — reload reproduces identical vegetation without persisting placement, except where brush edits apply (§5).

Procedural mesh builders target `MeshData` directly (not raw GL buffers):
```cpp
// VegetationPrimitives.h/.cpp
void addCylinder/addCone/addEllipsoid/addBox/addQuadPlane/addGrassBlade/addFlower(MeshSubmeshData&, ...);
MeshData buildSpeciesMesh(VegSpecies species, uint32_t variantSeed);
```
One `MeshData` per species, built once at `VkTerrainSubsystem::initialize()` and registered via `registerRuntimeOBJRaw` under asset ids like `"veg:pine"`; instances reuse that single GPU mesh through `InstancedMeshComponent::instanceTransforms`.

**ECS wiring**: one `InstancedMeshComponent` entity **per species per chunk** (not per instance), using the component's existing (currently-idle) frustum-clustering fields for high counts, and its existing `maxDrawDistance`/`instanceCullRadius` for LOD — no impostor mesh (the old system had unused impostor settings; don't replicate that).

**Interactive trees**: ~22% of Pine/Oak/Birch instances within `interactiveTreeChunkRadius` chunks of the camera (capped per chunk) get promoted to real ECS entities: `TreeComponent{health=3,...}` (struct exists, unused — first real caller) + `ColliderComponent{Capsule}` + static `RigidbodyComponent`, each with its own single-instance `MeshComponent`. Demotion back to plain instances happens on the same boundary-cross streaming pass, not every frame.

## 4. Collision Integration

Collision heights come from the **same** `sampleHeightGrid()` call as the visual mesh, at fixed LOD0 resolution (33 samples/edge) regardless of the chunk's visual LOD, generated only for chunks within `collisionChunkRadius` (default 2, smaller than `viewDistanceChunks`). Budget: `collisionUpdatesPerFrame` (default 2) caps `addTerrainChunk`/`removeTerrainChunk` calls per frame — a chunk's visual mesh can be live for a few frames before its physics body catches up (matches the old system's deferred-collision design).

Jolt heightfield shapes are **immutable** — a brush edit that changes heights requires full `removeTerrainChunk` + `addTerrainChunk`, not an in-place update; this cost is explicit and budgeted through the same queue as any other collision churn. Terrain bodies bypass `RigidbodyComponent`/`ColliderComponent` entirely (as the existing `addTerrainChunk` scaffolding already does) — they're static physics geometry paired 1:1 with a separate mesh-rendering ECS entity, not gameplay entities.

Phase 4 also removes `main.cpp`'s placeholder flat static Box floor (confirmed at `main.cpp:178-186`) and the `terrainAmplitude=0.12` flatness hack tied to it (`main.cpp:359`).

## 5. Terrain Brush / Editing Tool

**Centralized query + raycast API** — replaces the old system's naive fixed-step march duplicated across 3 call sites:

```cpp
class TerrainQuery {
public:
  float heightAt(glm::vec2) const;
  BiomeType biomeAt(glm::vec2) const;
  glm::vec3 normalAt(glm::vec2) const;   // 4-tap central difference over heightAt()
  float slopeAt(glm::vec2) const;
  bool isUnderwater(glm::vec2) const;
  bool isChunkLoadedAt(glm::vec2) const;
  glm::vec2 clampXZToLoadedRegion(glm::vec2) const;
  struct RaycastHit { bool hit; glm::vec3 point; glm::vec2 xz; float distance; };
  RaycastHit raycast(glm::vec3 origin, glm::vec3 dir, float maxDistance = 200.0f, float stepSize = 0.5f) const;
};
```
`raycast()` keeps the old 0.5m fixed-step march (simple, proven) plus one binary-search bisection refinement on the bracketing segment to reduce stair-step jitter — a small improvement, not a redesign. This single implementation serves the editor brush, any future gameplay interaction tool, and physics/debris queries — collapsing what was 3 independent copies in the old codebase.

**Height edits** — `HeightOffsetGrid`: sparse per-chunk float grids allocated lazily on first edit, radial linear falloff within brush radius, bilinearly sampled inside `computeHeight()` as the top layer over procedural noise (this is how edits composite with regenerated/streamed terrain rather than baking into a static mesh). Persist across unload/reload keyed by `ChunkCoord`; a stroke touching multiple chunks marks each dirty, driving the normal boundary-cross rebuild path (mesh **and** physics body, per §4's immutability note).

**Vegetation edits** — `AddVegetation`/`RemoveVegetation` brush modes: Remove deletes matching instances/entities within radius; Add uses area-uniform disc sampling (`sqrt(rand)*radius`) for `scatterCount` new instances at `heightAt()`. Both persist in a `PaintedInstanceCache` keyed by `ChunkCoord`, reapplied over the procedural placement result on every chunk rebuild.

```cpp
struct TerrainBrushSettings {
  bool enabled = false;
  enum class Mode { Raise, Lower, AddVegetation, RemoveVegetation } mode = Mode::Raise;
  VegSpecies vegTarget = VegSpecies::Grass;
  float radius = 6.0f, strength = 2.0f; int scatterCount = 6;
};
```

**Concurrency edge case**: if a brush edit lands on a chunk with an in-flight rebuild job, let that job finish and land, then immediately re-dirty and requeue — no attempt to cancel in-flight worker jobs (keeps the worker-pool contract simple).

## 6. Material / Shader Blending

Replace `terrain.task/.mesh/.frag` with:
```
shaders/vulkan/terrainChunk.frag   // new fragment shader
```
Chunk meshes reuse the **existing standard mesh vertex shader** (ordinary indexed geometry now, no mesh-shader vertex-stage needs). `terrainChunk.frag` ports the height/slope 4-layer blend (sand/grass/rock/snow) that the *current* terrain shader already implements — that part barely changes — adding a biome-tint multiply from `uv.y`'s biome band, active only when multi-biome mode is toggled on. Reuses the current shader's existing bindless detail-texture sampling and `rayQueryEXT` soft-shadow/SSAO paths unchanged (orthogonal to how geometry was produced). `MeshComponent::isTerrain=true` is what routes a chunk entity to this pipeline instead of the generic PBR mesh pipeline.

`TerrainMaterialSettings` ports the old settings **trimmed** to what this shader actually exposes (height/slope thresholds + 4 layer colors, matching current `VulkanRenderer::Params::terrainColor*` fields) plus a few new fields (per-layer roughness, macro-variation strength, biome-tint intensity). The old system's full triplanar-detail/glint/quality-tier apparatus is explicitly deferred to Phase 6 polish, not required for a functional terrain.

## 7. Editor Integration

`VkEditor::Context` (confirmed current fields: `scene, assets, physics, renderer, dt, simulatePhysics, assetDir`) gains one reference: `Terrain::TerrainChunkManager& terrain`. `drawEnvironment`'s existing "Terrain Generator"/"Terrain Materials" `CollapsingHeader`s (currently bound to `VulkanRenderer::Params`) get rebound to the new settings structs, following the exact same direct `ImGui::SliderFloat(&field)` pattern already in use — no viewmodel layer needed:

- **"Terrain Generator"**: seed, chunkWorldSize, heightScale, noiseFrequency, octaves/lacunarity/gain, macroStrength/mountainSpan, seaLevel, singleBiomeOnly toggle, viewDistanceChunks, plus a **"Regenerate All Chunks"** button (most fields require a full rebuild, unlike the old system's live GL uniforms).
- **"Terrain Materials"**: layer colors/thresholds, same binding pattern as today.
- **"Terrain Brush"** (new): enabled toggle, mode radio, radius/strength/scatterCount, species dropdown.
- **Statistics panel addition**: active chunk count / pending jobs / vegetation instance count / physics body count — cheap, useful for verifying streaming budgets during development.

## Terrain Generator Panel Implementation Notes (learned while building it, post-Phase-6)

Delivered as a standalone follow-up after all 6 phases, not as its own
numbered phase — closes the one substantial gap §7 above left open (the
Brush panel was already built in Phase 5; Statistics panel additions and a
Vegetation-specific panel are still not done).

**`TerrainChunkManager::shutdown()` + `init(newSettings)` turned out to
already be the exact "full teardown + rebuild" primitive needed** — no new
engine-core reset machinery was required. `shutdown()` was already
thread-safe (joins every worker thread before touching shared state) since
it's what the destructor itself calls.

**A real gap found before writing the reset code**: `VkTerrainSubsystem`'s
own `shutdown()` does *not* destroy ECS entities for active chunks (only
removes physics bodies) — fine for a real app shutdown (the whole scene is
going away with it), but wrong for `regenerate()`, which keeps the app
running. `regenerate()` needed a more thorough teardown loop (destroying
`mActive`/`mInteractiveTrees` entities too), mirroring the per-chunk unload
branch in `addFrameInstances()` rather than `shutdown()`'s lighter version.

**Brush edits are wiped on regenerate** via a new `HeightOffsetGrid::clear()`
(didn't exist before), called automatically from inside
`TerrainChunkManager::init()` itself rather than left for the caller to
remember — harmless no-op on first-ever startup.

**Self-verified live** (headless capture can't click UI buttons, so a
temporary debug env-var trigger was wired into `main.cpp`, exercised, then
fully reverted — confirmed via `git diff`): triggered `regenerate()`
mid-run with a new seed/heightScale/`singleBiomeOnly=false`. Confirmed no
crash, the terrain visibly changed shape (taller/steeper), entity count
stayed exactly the same (no duplication/leak — since the camera hadn't
moved, the new chunk manager re-requested the same coords, which correctly
hit `VkTerrainSubsystem`'s existing `updateMeshFromData()` in-place-refresh
branch rather than creating new mesh handles), and the 9 vegetation species
meshes refreshed cleanly (`VulkanAccel` log confirmed `Updated BLAS N in
place` for all of them). **Not verified**: actually clicking the panel's UI
controls live, or a regenerate where the camera *has* moved (a case more
likely to hit the accepted "orphaned mesh cache entry" limitation).

## Phase 1 Implementation Notes (learned while building it)

`registerRuntimeOBJRaw` turned out not to be usable as originally envisioned: it stores an opaque, already-GPU-uploaded `void*` and explicitly discards the CPU `MeshData` (`rec.cpu.reset()`), but `VulkanRenderSystem::update()` — the only code that turns an ECS `MeshComponent` into a draw — only ever reads the CPU side (`AssetManager::getOBJData()`), never `AssetManager::getOBJ()`. No `ModelGpuBackend` is registered anywhere in `VulkanRHI` either. So a mesh registered via `registerRuntimeOBJRaw` is invisible to the normal per-frame ECS→render path; it would need either a new `ModelGpuBackend` (nontrivial, not attempted here) or a `VulkanRenderSystem` special-case.

For Phase 1's single static chunk, `VkTerrainSubsystem` instead calls `VulkanRenderer::createMeshFromData()`/`addInstance()` directly — the same low-level calls `VulkanRenderSystem` itself makes per entity — and tracks the returned `MeshHandle` itself. Because `VulkanRenderer::addInstance()` is called every frame after `VulkanRenderSystem::update()` (which clears+rebuilds `mInstances` from the ECS each frame), `VkTerrainSubsystem::addFrameInstance()` is called right after it in `main.cpp`'s loop to re-add the terrain instance before `drawFrame()`. An ECS entity (`TerrainChunk_0_0`, `MeshComponent{isTerrain=true}`) is still created for Hierarchy/Inspector visibility, but it's otherwise inert for rendering.

Terrain routing to `terrainChunk.frag` was implemented via a `bool isTerrain` flag on `VulkanRenderer::Instance` (not `registerRuntimeOBJRaw`/`AssetManager`) — set by `addInstance(mesh, transform, isTerrain)`'s new third parameter, and used in `drawFrame()`'s main pass to partition `mInstances` into two draw loops (`mScenePipeline` vs. the new `mTerrainChunkPipeline`). The depth prepass needed no changes at all: terrain chunks share `mesh.vert`'s vertex layout, so the existing generic `mDepthPrepassPipeline` loop over all instances already covers them — the old mesh-shader terrain's separate `mDepthPrepassTerrainPipeline` was deleted outright rather than adapted.

`FrameDataGpu`'s `terrain`/`terrain2` vec4s (amplitude/frequency/octaves/seed/lacunarity/gain/heightOffset/warp — the fbm generator params, meaningless now that height comes from the CPU) were deleted from the C++ struct and from `mesh.vert`/`mesh.frag`'s GLSL declarations, shifting `mesh.frag`'s explicit `camPosWS` offset from 336 to 304. `terrainMat1`/`terrainMat2`/the 4 `terrainColor*` fields were deliberately **kept** (still packed from `VulkanRenderer::Params`) since `terrainChunk.frag` reuses them verbatim for its height/slope material blend — this means the existing "Terrain Materials" editor panel in `VkEditor.cpp` needed no changes at all; only the "Terrain Generator" (noise) panel was removed, since Phase 1 has no noise-generator UI yet (that's Phase 2+, once `TerrainChunkManager`/`TerrainSettings` are reachable from the editor).

Registered future work for Phase 2+: revisit whether `registerRuntimeOBJRaw` should be wired up properly (new `ModelGpuBackend` + `VulkanRenderSystem` change) once streaming needs it for load/unload lifecycle bookkeeping, or whether `VkTerrainSubsystem`'s direct-renderer-call pattern should just be extended to `TerrainChunkManager` wholesale (bypassing `AssetManager` for terrain permanently). The direct-call pattern already works and is simpler; the plan's original assumption that Phase 1 would exercise `registerRuntimeOBJRaw` was wrong and is corrected here.

## Phase 2 Implementation Notes (learned while building it)

**Unplanned renderer fix, found during design rather than assumed up front:** `VulkanRenderer::finalizeScene()` is not incremental — every call does `vkDeviceWaitIdle` + tears down **all** existing acceleration structures + rebuilds **one BLAS per mesh for every mesh ever created** (`VulkanRenderer.cpp`). Phase 1 only called it once at startup so this never surfaced. Naively calling it once per streamed-in chunk (or even once per frame that any chunk completed) would have meant a full-device-stall + full-BLAS-set rebuild every time, growing more expensive the longer a session ran — incompatible with "fly around and watch chunks stream in smoothly." Fixed by adding `VulkanAccel::appendBlas()` (builds BLAS only for the new tail meshes, no teardown, no stall) and `VulkanRenderer::growScene()` (the incremental counterpart to `finalizeScene()`, used by streaming instead of it). The per-frame instance/TLAS path was already fine as-is — `VulkanAccel::recordTlasBuild()` already rebuilds only the TLAS from the current instance list every frame, cheaply; only BLAS *creation* needed the incremental path. Also bumped the TLAS's initial instance capacity from a `max(2x, 256)` heuristic to `max(2x, 4096)` so streamed-in chunk instances (up to `(2*viewDistanceChunks+1)²` ≈ 169 at default settings) never force a TLAS recreate mid-stream. Verified live: smoke-run logs show `[VulkanAccel] Appended 4 BLAS (total 7)` / `(total 11)` rather than a full rebuild.

**Chunk coordinate convention fixed, not just extended:** Phase 1's single chunk passed `chunkOrigin=(0,0)` to `sampleHeightGrid` (which treats it as the chunk's *min corner* in world space) while placing the resulting chunk-local mesh (centered at its own origin, spanning `[-half, +half]`) with an *identity* transform — i.e., visually centered at world `(0,0)` while sampled as if its min corner were at world `(0,0)`. For one static chunk nobody could tell; for multiple tiling chunks this would have desynced height sampling from mesh placement and broken seams. Fixed by adding `chunkMinCorner()`/`chunkCenterWorld()`/`chunkCoordFromWorldXZ()` to `TerrainTypes.h` as the single source of truth for the convention (chunk `(x,z)` occupies world cell `[x*size,(x+1)*size) × [z*size,(z+1)*size)`), used consistently by both `TerrainChunkManager` (samples at the min corner) and `VkTerrainSubsystem` (places the instance transform at the center).

**Scope actually built, matching the plan's steps 1–3 and 5** (step 4, collision, is Phase 4 and was skipped): `TerrainChunkManager` owns a fixed-size `std::thread` worker pool (none existed anywhere in `Engine/` to reuse — confirmed by grep) and a `ChunkCoord → ChunkState` table; `streamUpdate()` does the boundary-cross early-out, Chebyshev-disc desired-set diff (nearest-first, 5 LOD bands via `((chunkResolution-1) >> lod) + 1` = 33/17/9/5/3), budgeted dispatch/drain, and unload queueing. A real concurrency bug was caught and fixed before it shipped: a chunk marked `Unloading` while its build job was still in flight would otherwise have its stale completed mesh pushed into the upload queue anyway (instancing it right after tearing it down) — the drain step now drops any completed result whose chunk isn't still `Building`. `TerrainChunkMesher` gained skirt geometry (a downward-facing curtain around the outer ring, wound to face outward using the same `edge1×edge2` convention as the top face) to hide LOD/seam cracks without cross-chunk vertex sharing, matching the plan's design.

**Known limitation, deliberately deferred:** unloading a chunk stops rendering it and destroys its ECS entity, but there's still no `destroyMesh()`/BLAS-removal path in `VulkanRenderer`, so the GPU mesh is never freed. Mitigated by caching mesh handles per `(ChunkCoord, lod)` in `VkTerrainSubsystem` so revisiting terrain reuses the existing mesh instead of rebuilding it — growth is bounded by distinct chunk+LOD combinations visited per session, not by load/unload event count. Full reclamation (index-stable BLAS compaction) is real work and was explicitly scoped out of Phase 2; flagged here as the next thing to tackle if long sessions show GPU memory growth.

**Not yet verified:** an actual interactive fly-around watching for visual popping/LOD seams/hitches at speed — automated headless smoke runs and the 79/79 test suite confirm the mechanics (correct chunk coordinates, incremental BLAS append, clean worker-pool shutdown over 300 frames) but can't drive live keyboard/mouse input. Do this manually via `run-glgenvk.bat` before considering Phase 2 fully closed out.

## Phase 3 Implementation Notes (learned while building it)

**Scope grew beyond the original design at the user's direction, not by drift.**
The original plan assumed vegetation would reuse `InstancedMeshComponent` (a
GL-era leftover with `unsigned int instanceVBO` fields, never read by any
Vulkan code) for per-species-per-chunk instancing. Investigation found **no
GPU-instanced draw path existed anywhere in the renderer** — every draw was
`vkCmdDrawIndexed(..., instanceCount=1, ...)`. Given that, and given
`TreeComponent`/`ColliderComponent`/`RigidbodyComponent` already existing with
a generic physics sync loop independent of Phase 4's terrain-heightfield work,
the user chose to build real GPU instancing now and include interactive trees
in this pass, rather than defer both. See the two `AskUserQuestion` decisions
at the start of this phase for the reasoning.

**Real GPU instancing added additively**, following the same pattern Phase 1/2
already established (new parallel pipeline, not a rewrite of the existing
one): `shaders/vulkan/meshInstanced.vert` is `mesh.vert` with the model matrix
sourced from a 2nd vertex binding (`VK_VERTEX_INPUT_RATE_INSTANCE`, 4 `vec4`
attributes) instead of the push constant; `mesh.frag` is reused verbatim.
`VulkanRenderer` gained `mVegetationPipeline`/`mVegetationDepthPrepassPipeline`
(structurally identical to `mScenePipeline`/`mDepthPrepassPipeline`, just the
new shader + vertex input) and a `VegSpeciesBuffer` per species (host-visible,
persistently-mapped, grows geometrically like a `std::vector`, never shrinks).
`setVegetationBatches()` is called once per frame; `drawFrame()` gained one
instanced draw call per species per submesh in both the depth prepass and
color pass — draw-call count is independent of placement count. Each
placement still gets a `VulkanAccel::InstanceInput` in the TLAS-instance list
(referencing its species' shared BLAS) so ray-traced shadows stay correct;
that only changes rasterization draw-call count, not the ray-tracing path.

**Found and fixed a real correctness gap, not introduced by this phase:**
`MaterialAsset::baseColor` was never consumed anywhere in
`VulkanRenderer.cpp` — `createMeshFromData()` only ever resolved
`textureIndex` from `texDiffusePath`, falling back to `mDefaultTexIndex` (a
visible **checkerboard** placeholder texture, not a flat color) whenever a
submesh had no texture path. This affected every procedural mesh in the
renderer already (terrain chunks, `MeshPrimitives.cpp`'s primitives), not just
vegetation — it just became visible once vegetation needed distinguishable
per-species colors. Fixed by synthesizing a cached 1×1 solid-color texture
from `baseColor` when `texDiffusePath` is empty (`VulkanRenderer.cpp`'s
`textureFor` lambda). Side effect: terrain chunks (which never set
`baseColor`, defaulting to white) now render their detail-texture layer as
flat white instead of the checkerboard placeholder — a visible but incidental
improvement, not a regression (the checkerboard was never an intentional
terrain look).

**Vegetation data flow matches the plan exactly**: `scatterVegetation()` hooks
into `TerrainChunkManager`'s worker job right after the dominant-biome
majority vote, using the same `heights`/noise set already in scope. One
deviation from the plan's literal signature: height per placement candidate
is resolved via a fresh `computeHeight()` call (not bilinear interpolation of
the already-sampled mesh grid) — candidate density per chunk is low enough
(tens to ~200) that this is cheap on the worker thread, and it keeps
`computeHeight()` as the exact single height authority rather than an
approximation of it. Vegetation is only scattered for LOD 0 (finest) chunks —
distant chunks don't need per-plant detail, matching the "no impostor mesh"
simplicity already called for.

**Interactive trees, without a rendering special-case**: a promoted tree gets
no `MeshComponent` at all — it stays purely visual as part of its species'
batch (so it isn't drawn twice, and doesn't need `AssetManager` mesh
resolution, sidestepping the exact dead-end Phase 1 hit with
`registerRuntimeOBJRaw`). Promotion spawns a co-located marker entity
(`TransformComponent` + `TreeComponent` + `RigidbodyComponent{Static}` +
`ColliderComponent{Capsule}`) picked up by `PhysicsSystem`'s existing generic
`view<RigidbodyComponent>()` sync loop — confirmed to already handle Capsule
shapes (`dimensions.x`=radius, `dimensions.y`=full height) independent of the
terrain-heightfield API. ~20% of eligible (Pine/Oak/Birch) instances within
`interactiveTreeChunkRadius` chunks of the camera are promoted (a fixed
1-in-5 stride rather than a random 22%, close enough to the plan's figure and
simpler); demoted on the same per-frame pass once their chunk falls out of
range or unloads.

**Not yet verified**: an actual interactive fly-around confirming forests
visually populate per-biome and that walking into a promoted tree collides —
same limitation as Phase 2, automated headless smoke runs and the 86/86 test
suite confirm the mechanics but can't drive live keyboard/mouse input.

## Phase 4 Implementation Notes (learned while building it)

**The scaffolding was already correct — the risk was in how it'd be
called, not in the API itself.** `PhysicsSystem::addTerrainChunk`/
`removeTerrainChunk` (`Engine/ECS/Systems/PhysicsTerrainChunk.cpp`) have had
zero call sites since Phase 2. Investigation confirmed the implementation
itself needs no changes — the risk flagged in this plan (a min-corner vs.
center mismatch) was about the *call site*, not the API: `chunkOrigin` is
Jolt's heightfield min-corner (`inOffset`), matching `TerrainTypes.h`'s
`chunkMinCorner()`, not `chunkCenterWorld()` (which `VkTerrainSubsystem`'s
nearby render-transform code uses for the same chunk). Both return a
`glm::vec2` of identical shape, so this was a real copy-paste trap, not a
theoretical one — the actual `updateTerrainCollision()` call site uses
`chunkMinCorner()` explicitly.

**Collision was decoupled from the mesh streaming pipeline entirely, by
design, not by oversight.** The original design's "step 4" implied slotting
collision into the same per-chunk load/unload event stream as mesh
upload. But `collisionChunkRadius` (2) is smaller than `viewDistanceChunks`
(6), so a chunk's collision eligibility changes as the camera moves *within*
the already-loaded view radius — an event with no natural home in
`TerrainChunkManager`'s upload/unload queues, which only fire once per
chunk's full lifecycle. Instead, `VkTerrainSubsystem::updateTerrainCollision()`
re-evaluates every currently-active chunk's distance to the camera every
frame, structured identically to Phase 3's `promoteInteractiveTrees()`
(demote pass, then promote pass, one shared `collisionUpdatesPerFrame`
budget counter) — proven to generalize cleanly to a second, unrelated
distance-gated per-chunk behavior. `TerrainChunkManager` gained exactly one
method, `sampleCollisionHeights()`, a synchronous main-thread-callable
wrapper around the same `sampleHeightGrid()` the worker jobs already use —
confirmed safe to call off the worker-thread pipeline since the noise set is
read-only per call. This keeps `TerrainChunkManager` fully physics-free, as
its own design comment already required.

**Confirmed, not assumed: Jolt's `HeightFieldShapeSettings` has no hard
constraint that would have blocked using the fixed `chunkResolution` (33)**
regardless of a chunk's visual LOD — only `sampleCount/blockSize >= 2` is
enforced (33/2=16), and Jolt pads internally to a block-size multiple,
transparent to the caller.

**Terrain bodies are confirmed structurally invisible to the generic
`RigidbodyComponent` physics sync** used by props and Phase 3's interactive
trees — `addTerrainChunk` never touches the ECS at all, so
`view<RigidbodyComponent>()` cannot iterate over a terrain body by
construction, not merely by convention.

**Scope came in smaller than the plan doc's original wording implied**: the
"remove ... the `terrainAmplitude=0.12` flatness hack" item turned out to be
stale — that value/hack no longer exists anywhere in `main.cpp` (the doc's
own cited line number pointed at unrelated camera-sensitivity code in the
current file). Only the placeholder `Floor` entity (a static Box collider
standing in for terrain) needed removing.

**Not yet verified**: an actual live walk/fall onto real terrain confirming
the physics shape matches the visual mesh, and flying near the
`collisionChunkRadius` boundary to watch for add/remove churn — same
limitation as Phases 2 and 3, automated headless smoke runs and the 88/88
test suite confirm the mechanics but can't drive live keyboard/mouse input.

## Phase 5 Implementation Notes (learned while building it)

**Scope was narrowed at the user's direction, not by drift**: height
sculpting (Raise/Lower) only this pass; vegetation Add/RemoveVegetation
brush modes are deferred (vegetation is fully procedural today, so
"painting" it needs its own persistence cache — `PaintedInstanceCache` — a
separable, similarly-sized follow-up). The `TerrainBrushSettings::Mode` enum
only has 2 values for now but is structured to extend without reshuffling.

**No dirty-chunk rebuild mechanism existed anywhere, confirmed by reading
the full state machine.** `TerrainChunkManager` only ever dispatched a job
for `ChunkState::Unloaded` chunks; there was no way to force an
already-`ReadyToUpload` chunk to rebuild short of a full unload/reload
round-trip. `TerrainChunkManager::applyHeightBrush()` fills this gap by
bypassing the `Unloaded`-only dispatch gate directly for touched chunks
found in that state, recomputing their LOD the same way `streamUpdate()`
does (from `mLastCameraChunk`).

**Investing in a real mesh/BLAS update-in-place API paid off cleanly,
per the user's decision**: it turned out simpler than a general free-list/
destroy system would have been, because a rebuild always reuses the *same*
handle — there's no "is this slot free for someone unrelated" bookkeeping to
build. `VulkanRenderer::updateMeshFromData()` was implemented by extracting
the existing `createMeshFromData()` body into a shared `buildMeshFromData()`
helper (used by both), and `VulkanAccel::updateBlas()` mirrors
`appendBlas()`'s existing `buildOneBlas()` reuse pattern but destroys-then-
rebuilds a single existing slot instead of appending. One real
synchronization risk found and handled: unlike `appendBlas()` (pure
addition, touches nothing existing), `updateBlas()` destroys a BLAS a prior
in-flight frame's TLAS might still reference — so, unlike every other
Phase 2/3 addition to this file, `updateBlas()` does call
`vkDeviceWaitIdle()` first (same justification `finalizeScene()`'s own
teardown already uses: rare, user-interaction-paced, a stall is acceptable
here).

**Confirmed the existing Jolt raycast is the wrong tool for brush picking,
not just "a different one would be nicer":** `PhysicsSystem::raycast()`
only hits chunks within `collisionChunkRadius` (smaller than the
editable/visible range), reports `entityId=0` for any terrain hit (no
`SetUserData` call in `addTerrainChunk`), and can't reflect a brush edit
until the next budgeted collision-rebuild pass. `TerrainQuery::raycast()`
(a fixed-step march against `heightAt()`, refined with one bisection pass)
works uniformly everywhere at full precision, reflecting edits the instant
they're applied. Also confirmed: **no screen-to-world ray reconstruction
existed anywhere in the codebase** (gameplay raycasts all use camera-forward
rays, never a mouse-position-derived one) — built from scratch in
`VkEditor::updateTerrainBrush()` using standard inverse-view-projection
unprojection, accounting for `GLM_FORCE_DEPTH_ZERO_TO_ONE` (defined
project-wide) putting the near plane at NDC z=0, not the OpenGL-default -1.

**An edit applied to an already-collision-active chunk needs its own
refresh path**, independent of `updateTerrainCollision()`'s distance-based
promote/demote pass (which only reacts to chunks crossing the
`collisionChunkRadius` boundary, never ones edited in place while already
inside it). Handled directly in `addFrameInstances()`'s upload loop: if a
rebuilt chunk already has a `physicsBodyId`, it's unconditionally
removed-and-re-added with fresh `sampleCollisionHeights()` output, since
Jolt heightfields are immutable (confirmed already in Phase 4).

**Not yet verified**: an actual live sculpting session — open the Terrain
Brush panel, raise/lower terrain near the player, confirm the mesh updates
within a frame or two, confirm collision follows, and confirm an edit
survives flying away (unloading the chunk) and back. Same limitation as
every prior phase: automated headless smoke runs and the 94/94 test suite
confirm the mechanics but can't drive live mouse/keyboard input.

## Phase 6 Implementation Notes (learned while building it — final phase)

**Confirmed before writing any shader code: no specular/roughness term
exists anywhere in `terrainChunk.frag`'s lighting** — it's exactly
`albedo * (ambient + (1-ambient)*direct)`, pure Lambertian, confirmed by
reading `main()` in full. The plan's original "per-layer roughness" field
was dropped rather than added as dead data — per this session's decision,
adding a real specular term would be a lighting-model change, not material
polish, and stayed out of scope.

**`vUV.y` (the biome band) really was already fully plumbed and just
unread** — `TerrainChunkMesher.cpp` has encoded it per-vertex since Phase 1
(`int(biome)/5.0f`), and the shader already declared it as an unused
varying with a comment pointing at this exact phase. No mesher/vertex-
shader changes were needed at all; only `terrainChunk.frag`'s `main()`
needed the decode-and-tint step.

**`FrameDataGpu`'s append-only extension pattern (established Phase 1,
reused Phases 2-3) held up for a 4th addition with zero friction**: one
`terrainMat3` vec4 appended after `skyAmbientParams`, only
`terrainChunk.frag` declares it, `mesh.vert`/`mesh.frag`/
`meshInstanced.vert` untouched.

**Self-verification method**: rather than waiting for a live editor session
to confirm the shader math renders correctly, temporarily flipped the new
defaults on (`terrainBiomeTintEnabled=true`, amplified macro/rock strength)
plus `TerrainSettings::singleBiomeOnly=false` (to get real biome variety
across chunks, not just a uniform Plains band), rebuilt, captured a headless
smoke screenshot, confirmed no crashes/validation errors and a real state
change (entity/body counts jumped 18/7 → 43/32, confirming multi-biome
vegetation actually kicked in), then reverted every default back exactly
and rebuilt again — confirmed via `git diff` showing only the intended
`false`/`0.35`/`0.15`/`0.3` defaults remain. This doesn't replace an actual
live visual check (grazing camera angle in the headless capture made the
terrain surface itself hard to judge), but it did rule out shader
compilation/runtime errors and confirmed the data plumbing (`Params` →
`FrameDataGpu` → shader) is live end-to-end.

**Not yet verified**: an actual live visual tuning pass — open the Terrain
Materials panel, toggle Biome Tint on with `singleBiomeOnly` disabled,
adjust Macro Variation and Rock Detail sliders, and take the plan's own
"before/after screenshots" for this phase's demo goal. Same limitation as
every prior phase: headless smoke runs confirm the mechanics but can't
judge the actual visual result the way a human eye can.

## 8. Phased Delivery Plan

| Phase | Scope | Risk | Demo |
|---|---|---|---|
| 0 — Foundation ✅ | `TerrainNoise`, `TerrainQuery` (no raycast yet), no ECS/renderer dependency | Low | Done: `Tests/test_terrain_noise.cpp`, 10 cases, all passing |
| 1 — Static single chunk ✅ | `TerrainChunkMesher` builds one fixed 128×128 chunk, rendered with `terrainChunk.frag` via `VkTerrainSubsystem`. Deleted `terrain.task/mesh/frag`, `createTerrainPipeline()`, `kTerrainPatches`, mesh-shader-only `Params`/`FrameDataGpu` fields | Medium | Done: headless smoke run, `TerrainChunk_0_0` renders with correct shading/shadows/materials, 73/73 tests still pass |
| 2 — Streaming + LOD ✅ | `TerrainChunkManager` worker pool, chunk state machine, budgets, `VkTerrainSubsystem` wiring, LOD + skirts. Also required an unplanned renderer fix: incremental BLAS append (`VulkanAccel::appendBlas`/`VulkanRenderer::growScene`) | **Highest** (first multithreaded subsystem touching ECS/AssetManager — main-thread-only mutation boundary must hold) | Done: 79/79 tests pass, headless smoke runs confirm chunks stream in with correct coords and incremental (non-stalling) BLAS append; **live interactive fly-around not yet done** (needs a human at the keyboard) |
| 3 — Vegetation ✅ | `TerrainVegetation`, `VegetationPrimitives`, real GPU instancing (not planned originally), interactive trees included | Medium (became higher: no GPU instancing existed anywhere in the renderer beforehand) | Done: 86/86 tests pass, headless smoke confirms 9 species meshes register + instance/appended BLAS correctly, stable over a 300-frame run; **live interactive fly-around and tree-collision check not yet done** |
| 4 — Collision ✅ | Wire `addTerrainChunk`/`removeTerrainChunk` (were fully implemented, zero callers, since Phase 2), remove placeholder floor (the `terrainAmplitude` flatness hack the original doc cites no longer exists in `main.cpp`) | Medium (Jolt heightfield cost, immutable-shape-on-edit) | Done: 88/88 tests pass, headless smoke confirms the Floor entity is gone and no crashes over a 300-frame run; **live walk/fall-on-terrain check not yet done** |
| 5 — Brush editing (height only) ✅ | `HeightOffsetGrid`, `TerrainQuery::raycast()`, editor Brush panel, dirty-chunk rebuild, real mesh/BLAS update-in-place API (not planned originally). `PaintedInstanceCache`/vegetation brush modes deferred as a separable follow-up | Medium-high (streaming/edit concurrency edge cases; became higher: no mesh/BLAS replace-in-place path existed anywhere in the renderer) | Done: 94/94 tests pass, headless smoke confirms no regressions over a 300-frame run; **live sculpt/collision/persistence check not yet done** |
| 6 — Material polish ✅ | Biome-tint multiply (decodes the existing `vUV.y` biome band), macro-variation (smooth value noise), rock Worley detail. Per-layer roughness dropped (no specular/roughness term exists anywhere in this shader's lighting model to consume it) | Low | Done: builds clean, headless smoke confirms no regressions at defaults (tint off) and no crashes/artifacts with all 3 effects at amplified strength + real multi-biome variety enabled (self-verified: entity/body counts correctly jumped 18/7 → 43/32 under `singleBiomeOnly=false`, then reverted); **live visual tuning/before-after screenshots not yet done** |

## 9. New/Modified Files (execution checklist, for later)

*Status as of end of Phase 6 (all 6 phases in the delivery table done):
everything below is done (✅) except vegetation brush modes
(`PaintedInstanceCache`, deferred as a separable follow-up) and the
streaming-settings/"Terrain Generator" editor panel (§7's noise/seed/
regenerate-all controls — never built; explicitly out of every phase's
scope as written, would be its own follow-up). `VulkanRHI/VulkanAccel.h/.cpp`
(Phase 2), `shaders/vulkan/meshInstanced.vert` + vegetation pipeline
additions (Phase 3), `VulkanRenderer`'s `updateMeshFromData()`/
`VulkanAccel::updateBlas()` (Phase 5), and `FrameDataGpu`'s `terrainMat3`
(Phase 6) weren't anticipated when this checklist was first written — see
the per-phase Implementation Notes above for each. Phase 4 needed no new
files at all.*

**New:**
```
Engine/Terrain/TerrainTypes.h            ✅ (Phase 0/2/3)
Engine/Terrain/TerrainSettings.h         ✅ (Phase 0/3)
Engine/Terrain/TerrainNoise.h/.cpp       ✅ (Phase 0)
Engine/Terrain/HeightOffsetGrid.h/.cpp   ✅ (Phase 0 stub, real implementation Phase 5)
Engine/Terrain/TerrainChunkMesher.h/.cpp ✅ (Phase 1, skirts added Phase 2)
Engine/Terrain/VegetationPrimitives.h/.cpp ✅ (Phase 3)
Engine/Terrain/TerrainVegetation.h/.cpp  ✅ (Phase 3)
Engine/Terrain/TerrainQuery.h/.cpp       ✅ (Phase 0, raycast() added Phase 5)
Engine/Terrain/TerrainChunkManager.h/.cpp ✅ (Phase 2, vegetation hook added Phase 3, sampleCollisionHeights() added Phase 4, applyHeightBrush() added Phase 5)
VulkanRHI/runtime/subsystems/VkTerrainSubsystem.h/.cpp ✅ (Phase 1, rewritten Phase 2, vegetation/trees added Phase 3, collision promote/demote added Phase 4, brush passthrough added Phase 5)
shaders/vulkan/terrainChunk.frag         ✅ (Phase 1, biome-tint/macro-variation/rock-detail added Phase 6)
shaders/vulkan/meshInstanced.vert        ✅ (Phase 3, not originally planned)
```

**Modified:**
```
VulkanRHI/runtime/VkEditor.h/.cpp           — Context gains terrain ref ✅ (Phase 5); Terrain Brush panel ✅ (Phase 5); biome-tint/macro-variation/rock-detail sliders in Terrain Materials ✅ (Phase 6); streaming-settings/"Terrain Generator" panel still not done (§7, no phase claims it)
VulkanRHI/runtime/VkAppState.h              — add TerrainChunkManager member; fix stale comment ✅ (Phase 2)
VulkanRHI/runtime/subsystems/VkCoreAppLayer.cpp — add state.terrain.streamUpdate(...) call ✅ (Phase 2)
VulkanRHI/runtime/main.cpp                  — register VkTerrainSubsystem ✅ (Phase 1); remove placeholder floor ✅ (Phase 4); addFrameInstances() ✅ (Phase 2); pass terrainSubsystem into editor Context ✅ (Phase 5)
VulkanRHI/VulkanRenderer.h/.cpp             — remove kTerrainPatches/terrain* Params/createTerrainPipeline/mesh-shader draw calls ✅ (Phase 1); add isTerrain pipeline routing ✅ (Phase 1); add growScene()/bump TLAS capacity ✅ (Phase 2, not originally planned); add instanced pipelines/setVegetationBatches()/baseColor-fallback-texture fix ✅ (Phase 3, not originally planned); add updateMeshFromData() ✅ (Phase 5, not originally planned); add biome-tint/macro-variation/rock-detail Params + terrainMat3 ✅ (Phase 6)
VulkanRHI/VulkanAccel.h/.cpp                — add appendBlas() ✅ (Phase 2, not originally planned); add updateBlas() ✅ (Phase 5, not originally planned)
Engine/ECS/Components.h                     — no struct changes needed; isTerrain ✅ (Phase 1), TreeComponent/ColliderComponent/RigidbodyComponent get first real callers ✅ (Phase 3); useTerrainShading (InstancedMeshComponent) still unused -- Phase 3 used real GPU instancing instead, not this GL-era component
```

**Critical files to re-read before implementation starts:** `Engine/Terrain/TerrainChunkManager.h/.cpp` (once it exists — highest-risk piece), `Engine/Assets/AssetManager.h` (`registerRuntimeOBJRaw`), `Engine/ECS/Systems/PhysicsTerrainChunk.cpp` (`addTerrainChunk`/`removeTerrainChunk`), `VulkanRHI/runtime/subsystems/VkPhysicsSubsystem.h/.cpp` (pattern to clone), `VulkanRHI/VulkanRenderer.h/.cpp` (terrain code to delete + pipeline routing to add).

## Verification (once implementation begins)

- Phase 0: standalone debug harness printing `heightAt()`/`biomeAt()` across a grid, sanity-check against expected macro shape (mountains near mountainMask peaks, oceans below seaLevel).
- Phase 1: run `glGenVk`, confirm single terrain chunk renders with correct height/slope material bands; `GLGEN_SMOKE_CAPTURE` PNG diff against current terrain for a rough visual sanity check.
- Phase 2: fly the free-cam at speed across chunk boundaries, watch the Statistics panel's chunk/job counts stay within configured budgets, visually confirm no seam cracks or LOD popping artifacts.
- Phase 3: watch vegetation populate/clear across several chunk loads/unloads; confirm entity counts return to baseline after flying away (no leaks).
- Phase 4: walk and fall onto sloped/hilly terrain in the player controller; confirm physics shape matches the visual mesh.
- Phase 5: sculpt terrain and add/remove vegetation via the editor brush, fly away and back, confirm edits persisted; edit a chunk mid-rebuild to exercise the requeue path.
- Phase 6: side-by-side screenshots before/after material polish.

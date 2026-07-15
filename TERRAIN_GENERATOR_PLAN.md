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

## 8. Phased Delivery Plan

| Phase | Scope | Risk | Demo |
|---|---|---|---|
| 0 — Foundation | `TerrainNoise`, `TerrainQuery` (no raycast yet), no ECS/renderer dependency | Low | Debug-print `heightAt()` samples, validate curve shape |
| 1 — Static single chunk | `TerrainChunkMesher` builds one fixed 128×128 chunk via `registerRuntimeOBJRaw`, rendered with `terrainChunk.frag`. Delete `terrain.task/mesh`, `createTerrainPipeline()`, `kTerrainPatches`, terrain `Params` fields | Medium (first real use of `registerRuntimeOBJRaw` + `isTerrain` routing) | Visual parity with current terrain, now CPU-mesh-based |
| 2 — Streaming + LOD | `TerrainChunkManager` worker pool, chunk state machine, budgets, `VkTerrainSubsystem` wiring, LOD + skirts | **Highest** (first multithreaded subsystem touching ECS/AssetManager — main-thread-only mutation boundary must hold) | Fly around, watch chunks stream with visible LOD, no seam cracks |
| 3 — Vegetation | `TerrainVegetation`, `VegetationPrimitives`, per-species instancing, no interactive trees yet | Medium | Biome-driven forests/deserts populate and clean up without entity leaks |
| 4 — Collision | Wire `addTerrainChunk`/`removeTerrainChunk`, remove placeholder floor + flatness hack | Medium (Jolt heightfield cost, immutable-shape-on-edit) | Walk/fall on real terrain shape |
| 5 — Brush editing | `HeightOffsetGrid`, `PaintedInstanceCache`, `TerrainQuery::raycast()`, editor Brush panel, dirty-chunk rebuild | Medium-high (streaming/edit concurrency edge cases) | Sculpt + plant/remove vegetation live, edits survive unload/reload |
| 6 — Material polish | Multi-biome blend toggle, macro-variation, optional Worley jitter for rocks | Low | Before/after screenshots |

## 9. New/Modified Files (execution checklist, for later)

**New:**
```
Engine/Terrain/TerrainTypes.h
Engine/Terrain/TerrainSettings.h
Engine/Terrain/TerrainNoise.h/.cpp
Engine/Terrain/HeightOffsetGrid.h/.cpp
Engine/Terrain/TerrainChunkMesher.h/.cpp
Engine/Terrain/VegetationPrimitives.h/.cpp
Engine/Terrain/TerrainVegetation.h/.cpp
Engine/Terrain/TerrainQuery.h/.cpp
Engine/Terrain/TerrainChunkManager.h/.cpp
VulkanRHI/runtime/subsystems/VkTerrainSubsystem.h/.cpp
shaders/vulkan/terrainChunk.frag
```

**Modified:**
```
VulkanRHI/runtime/VkEditor.h/.cpp           — Context gains terrain ref; new/rebound panels
VulkanRHI/runtime/VkAppState.h              — add Terrain::TerrainChunkManager member; fix stale comment
VulkanRHI/runtime/subsystems/VkCoreAppLayer.cpp — add state.terrain.streamUpdate(...) call
VulkanRHI/runtime/main.cpp                  — register VkTerrainSubsystem; remove placeholder floor (Phase 4); remove terrain Params defaults (Phase 1)
VulkanRHI/VulkanRenderer.h/.cpp             — remove kTerrainPatches/terrain* Params/createTerrainPipeline/mesh-shader draw calls; add isTerrain pipeline routing
Engine/ECS/Components.h                     — no struct changes; isTerrain/useTerrainShading/TreeComponent get first real callers
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

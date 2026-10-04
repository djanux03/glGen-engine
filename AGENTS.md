# Working on glGen

Orientation for an AI (or a new human) making changes to this codebase. For
*authoring content* with the engine rather than modifying it, see
`Tools/glgen-mcp/README.md` — that is a different job with different tools.

## What this is

A Vulkan game engine: ray-traced shadows, streamed procedural terrain, ECS,
Jolt physics, Lua scripting, an ImGui editor. On top of it sits an AI-first
asset pipeline — assets are described as parameters, generated at runtime, and
reviewed by rendering them. `AI_ASSET_PIPELINE_PLAN.md` is the design record
for that half and explains *why* most of it is shaped the way it is.

OpenGL has been removed. Vulkan is the only renderer.

## Build and test

```bash
# engine + editor  (needs the Vulkan SDK; glslc must be on PATH)
cmake -S . -B Build-vs18 -G "Visual Studio 18 2026" -A x64
cmake --build Build-vs18 --config Release --target glGenVk

# tests (doctest; standalone project, does NOT link the engine library)
cmake -S Tests -B Build-tests
cmake --build Build-tests --config Release
Build-tests/Release/glgen_tests.exe
```

The test target compiles individual engine `.cpp` files rather than linking
`EngineCore`, to stay free of GLFW/Vulkan. Adding a test that needs a new
engine source means adding that source to `Tests/CMakeLists.txt`.

### Running headlessly

The runtime takes environment variables so it can be driven without a mouse.
These are how you verify a rendering change without asking a human to look:

| Variable | Effect |
|---|---|
| `GLGEN_SMOKE_FRAMES=N` | render N frames and exit |
| `GLGEN_SMOKE_CAPTURE=path.png` | write a PNG on the last frame |
| `GLGEN_SMOKE_CAM=x,y,z,pitch[,yaw]` | place the camera |
| `GLGEN_GEN_SHOWCASE=<filter>` | line recipes up against open sky for review |
| `GLGEN_SCRIPT=file.lua` | run a Lua file at startup |
| `GLGEN_AGENT_PORT=8787` | open the JSON-RPC command port |
| `GLGEN_BACKGROUND=1` | render in a hidden window without stealing focus or capturing the mouse; smoke runs use this automatically |
| `GLGEN_BACKGROUND_FPS=N` | cap background rendering at N fps (default 15), to leave GPU time for other apps |

The woodland performance harness starts its own hidden engine and command port:
`python Tools/glgen-regress/woodland_perf.py` (add `--restore-scene` to verify saved
scene migration). It writes fixed shoreline/marsh captures, GPU pass timings and
vegetation triangle counts. Background pacing is excluded from GPU timings; live
GPU workloads can still preempt the measured work, so compare under the same load.
The editor Statistics panel and `render.stats()` expose the same timings.
Add `--visual-review --temporal-check` for open-sky/shoreline views and a measured
AA-off/on comparison of consecutive-frame cloud noise plus hidden resize validation.
`--sync-validation` enables the SDK's synchronization checks (timings are affected).
`--asset-regression` runs the recipe regression through this same hidden process.

Scatter layers can provide `meshLods`: ordered `{mesh, distance}` entries in
metres, selected per culling cell from the current camera. Depth, scene and ordinary
shadow casters use the same policy. An optional `shadowMesh` supplies casting-only
geometry; stock meadow grass disables casting and uses a broad, density-based terrain contact mask.
Keep small culling cells for dense layers; chunk-wide
bounds otherwise hold an entire nearby chunk at full detail. Placement is unchanged
by render LOD. Layers sharing a mesh remain separate batches with their own settings.

The authored woodland uses the same `WoodlandLayout.h` masks for service tracks,
rocky rises, and scatter exclusions. Terrain UV.x carries track coverage in this
profile (textures project from world position); `terrainTiling1.y` selects this
surface policy. Other profiles keep their procedural UV/material policy. The five
woodland slots are moss, forest litter, gravel, bedrock and wet soil. Manifest
`fixedPlacements` rows `[x,z,yawDegrees,scale]` bypass density/biome gates and use
half-open chunk ownership, so landmarks neither move with density nor duplicate
at borders. Props use shared instanced meshes like the vegetation.


Streamed terrain markers and promoted tree/rock colliders are `TransientComponent`
entities: terrain owns their lifetime, and scene snapshots preserve them in memory
while omitting them from disk. `ScenePersistence.h` migrates legacy generated proxies
by reserved name and structure, preserving authored assets, scripts and hierarchies.

**Render a capture and actually look at it** before claiming a visual change
works. Several bugs in this codebase's history were invisible in code review
and obvious in a screenshot.

## Layout

```
Engine/            EngineCore — no graphics API, links no Vulkan
  Assets/          MeshData (the CPU asset IR), parsers, AssetManager
  Generators/      procedural pipeline: recipes, generators, textures, validation
  Bridge/          CommandServer — JSON-RPC over localhost
  Scripting/       Lua bindings (sol2)
  Terrain/         chunked procedural terrain + scatter
  ECS/  Scene/  Core/  Input/
VulkanRHI/         the renderer
  runtime/         glGenVk: main loop, editor, subsystems, agent bridge
Tools/             glgen-client, glgen-mcp, glgen-fetch, glgen-regress (Python)
Tests/             doctest suite + golden images
```

**The layering rule:** `EngineCore` must never depend on `VulkanRHI`. Anything
needing the renderer registers itself from above — see
`ScriptSystem::addBindingHook` and `VkScriptBindings`, and the same split
between `CommandServer` (transport, engine-agnostic) and `VkAgentBridge`
(handlers, engine-aware).

## Invariants worth knowing before you change things

**Threading.** `Registry`, `AssetManager`, `Scene` and `VulkanRenderer` are
single-threaded with no locking. The command port's socket thread only parses
and enqueues; handlers run on the main thread at one fixed point per frame.
Anything that touches engine state from another thread is a bug.

**`AssetHandle::generation` vs `OBJRecord::contentVersion`.** `generation`
tracks handle *identity* (slot reuse — bumping it invalidates outstanding
handles). `contentVersion` tracks *content* (same asset, new bytes — handles
stay valid). Regeneration bumps the second. Conflating them makes meshes vanish
when they are regenerated.

**Generators must be deterministic.** Same generator, params and seed must give
byte-identical geometry, or recipes stop being reproducible. Use `GenRandom`;
do not use `std::uniform_real_distribution`, whose mapping is not standardized
across standard libraries.

**One acceleration structure per unique mesh.** Scattered content multiplies
every triangle by its instance count. Prefer few meshes with per-instance
variation over many unique meshes. Grass uses shared mesh variants and LODs, with baked world-space patch contact shading.
Grass blade casting is disabled by default to keep blades out of the TLAS. The
terrain grass-shadow switch can disable casting without disabling reception.

**Foliage materials.** Both mesh shaders support alpha cutouts and tangent-space
normal maps; depth alpha testing and CPU ray micromaps preserve leaf gaps in
shadows. Birch twig sprays and tiled bark are documented in
`assets/trees/birch/texture_provenance.json`. Grass must bypass tree-crown normal
rounding, and only green foliage receives transmission (bark must not glow).

**Temporal AA.** The woodland opts into a scene-linear resolve before bloom,
tonemapping and UI. History alpha stores depth; neighbourhood clipping and depth
rejection limit trails on wind-driven foliage. Camera cuts, resize, diagnostics
and disabling AA invalidate history. Both history images are initialized and
cross-frame read/write barriers stay on the graphics queue; never add CPU waits.

**Reproducible captures need the clock pinned.** Cloud drift, dew twinkle and
volumetric turbulence key off elapsed time; `Params::fixedTimeSeconds` freezes
them. The turntable also lifts its subject past the far plane so the streaming
world is not in frame, and disables the camera grade, which eases on the real
clock.

## After changing generators or shaders

```bash
GLGEN_BACKGROUND=1 GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe &
python Tools/glgen-regress/regress.py
```

Metrics catch geometry changes exactly; golden images catch shading changes.
If a difference is intended, re-run with `--update` — and look at the diff
before accepting it.

## House style

Match the surrounding code: the comment density here is high and deliberate.
Comments explain **why**, especially where a choice looks arbitrary or where a
simpler-looking alternative was tried and rejected. When you fix a subtle bug,
leave a line saying what the failure mode was — several comments in this
codebase exist because the bug came back otherwise.

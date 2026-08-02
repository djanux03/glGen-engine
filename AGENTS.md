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
variation over many unique meshes. Grass is deliberately excluded from the TLAS
entirely.

**Two renderer gaps, currently worked around rather than fixed.** `mesh.frag`
has **no alpha cutout** (`MaterialAsset::alphaCutoff` is silently ignored, so
an alpha-masked quad renders opaque — foliage silhouettes are therefore real
geometry) and **no normal-map binding** (only terrain material slots have one).
Closing either would improve foliage and surface detail broadly, and would let
`TextureGen` stop skipping normal maps.

**Reproducible captures need the clock pinned.** Cloud drift, dew twinkle and
volumetric turbulence key off elapsed time; `Params::fixedTimeSeconds` freezes
them. The turntable also lifts its subject past the far plane so the streaming
world is not in frame, and disables the camera grade, which eases on the real
clock.

## After changing generators or shaders

```bash
GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe &
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

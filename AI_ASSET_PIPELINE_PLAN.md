# AI-First Asset Generation Pipeline — Design & Roadmap

Target: **100% of glGen's 3D content (trees, rocks, props, materials, textures)
authored by AI from text descriptions, with no Blender in the loop.**

Written against the `vulkan-engine` branch (OpenGL removed, `glGenVk` is the
only runtime). Section 1 is what the codebase can do today; sections 2–3 are
the design; section 4 is the implementation order.

---

## 1. Codebase analysis — how ready is glGen?

Short answer: **the renderer is ready, the asset layer is not, and the
scripting layer is nearly empty.** The gaps are small and well-defined.

### 1.1 What already works in your favour

**`MeshData` is a proper backend-neutral asset IR** (`Engine/Assets/MeshData.h`).
This is the single most important thing you have. It carries:

- per-submesh vertices/indices + AABBs
- a full `MaterialAsset` per submesh (`Engine/Rendering/Material.h`): baseColor,
  roughness, metallic, AO, emissive, alphaCutoff, 7 texture slots with
  per-channel selectors, gloss-vs-roughness flag
- **`std::vector<MeshImage> images`** — embedded, decoded RGBA pixel payloads
  keyed by the same string the material's `tex*Path` uses
- `recenter(BaseY|Center)` and `rotateEulerDeg()` — pivot and up-axis fixups
  already implemented

Anything that can produce a `MeshData` is a first-class glGen asset. That is
your integration point, and it's already the right shape.

**The Vulkan renderer has a complete runtime mesh-streaming API**
(`VulkanRHI/VulkanRenderer.h`):

| API | What it gives you |
|---|---|
| `createMeshFromData(const ::MeshData&, name)` | in-memory mesh → GPU, returns `MeshHandle` |
| `updateMeshFromData(handle, data)` | **in-place** geometry replacement, handle stays valid, BLAS rebuilt at the same slot |
| `destroyMesh(handle)` | deferred destroy + slot recycling |
| `growScene()` | incremental BLAS build, **no `vkDeviceWaitIdle` stall** |
| `requestCapture(path)` | next frame writes a PNG |

Terrain streaming already drives all of this every frame
(`VkTerrainSubsystem.cpp`), so runtime mesh creation/replacement is a
*proven* path, not a theoretical one.

**Embedded textures upload with zero renderer changes.** Verified in
`VulkanRenderer.cpp:2965` — `buildMeshFromData()`'s `textureFor()` lambda
checks `data.findImage(path)` *before* touching the filesystem, expands
1/3/4-channel payloads to RGBA, and pushes them into the bindless array
(`VulkanBindless`, 1024 slots, update-after-bind). **A procedurally generated
texture never has to exist as a file.** There's also a cached solid-colour
fallback driven by `MaterialAsset::baseColor`.

**`ScatterManifest` is the precedent for AI-authored content**
(`Engine/Terrain/ScatterManifest.h`). It is a versioned JSON document with ~35
authorable fields per layer (density, per-biome multipliers, spacing,
scale/height/lean ranges, slope/moisture gates, clustering, wind, cull cell
size, tint, up-axis fix, collision type). An LLM can write a valid
`terrain_scatter.json` **today** and the engine consumes it. This is exactly
the interaction model the rest of the pipeline should copy.

**A headless render-and-screenshot harness already exists.** `runtime/main.cpp`
supports `GLGEN_SMOKE_FRAMES`, `GLGEN_SMOKE_CAPTURE`, `GLGEN_SMOKE_CAM`
("x,y,z,pitch[,yaw]"), plus a dozen scene/lighting presets. That is 80% of
"AI renders an asset and looks at it" — it just runs at process granularity
instead of being a request you can issue to a live editor.

**Also present:** `nlohmann::json` vendored (`ThirdParty/include/json.hpp`),
Lua 5.4 + sol2 with per-entity sandboxed environments, Scene JSON
save/load *including string variants* (`serializeToString`/`loadFromString`),
`PerlinNoise.h` in Core, Jolt collider auto-derivation, and an
`AssetManager::pollHotReload()` mechanism.

### 1.2 The gaps

| # | Gap | Detail |
|---|---|---|
| **G1** | **No in-memory asset registration** | `AssetManager` can only produce `MeshData` from a *file path* or the hardcoded `__primitive_*` switch (`MeshPrimitives.cpp`). `registerRuntimeOBJRaw()` looks like the hook but **isn't**: it takes a GPU `OBJModel*` (a class deleted on this branch), discards the CPU `MeshData` the render system needs, and **has zero callers**. This is the blocker for everything else. |
| **G2** | **Mesh cache never invalidates** | `VulkanRenderSystem::mMeshByAsset` caches `assetId → MeshHandle` permanently. Regenerating an asset under the same id silently keeps the old geometry, even though `updateMeshFromData()` exists to do it properly. |
| **G3** | **Lua has no asset/material/scene API** | `ScriptBindings.h` (347 lines) exposes only: `Vec3`, entity transform/velocity/impulse getters+setters, `world.spawn_rock`, `world.spawn_primitive`, `world.find_entity`, `world.destroy`, `input.*`, `log.*`, `physics.raycast`. There is no way to build a mesh, set a material, load an asset, or save a scene from script. |
| **G4** | **`ScriptSystem` is file-and-entity-only** | No `execString()`. Scripts run per-entity via `ScriptComponent`. There is no console/eval path. (Also: `initialize()` opens `sol::lib::io` and `sol::lib::os` — must not be in an agent sandbox.) |
| **G5** | **No HTTP client anywhere** | Nothing in `ThirdParty/` can make a network request. Option A cannot be done in-process without adding a dependency. |
| **G6** | **`PrimitiveMeshGenerator.h` is dead** | Includes the deleted `Engine/Assets/OBJModel.h`; nothing includes it. The live primitive set is `makePrimitiveMesh()` — 5 shapes, **non-indexed**, parameterless beyond segment counts. |
| **G7** | **Capture is process-scoped** | `requestCapture()` is fired from an env-var check at `frame == maxFrames-1`. Not a request/response you can issue to a running editor. |
| **G8** | **No procedural texture generation** | Nothing generates image data. `PerlinNoise.h` exists and is reusable, but nothing turns noise into a `MeshImage`. |

**Verdict:** the hard parts (bindless textures, in-place mesh replacement,
incremental BLAS, ray-traced shadows tracking dynamic geometry, headless
capture) are done. The missing pieces are plumbing and API surface — roughly
2–4 weeks of focused work, not a rewrite.

---

## 2. Evaluating the options

### Option A — Text-to-3D / Image-to-3D services

**Verdict: keep it, but as a secondary *offline* path, not the foundation.**

Against it, specifically for *this* engine:

- Output is high-poly, arbitrary topology, inconsistent scale/pivot/up-axis.
  Your renderer builds **one BLAS per unique mesh** and instances vegetation by
  the ten-thousand — dropping raw 40k-tri text-to-3D output into
  `setVegetationBatches()` is a frame-time cliff.
- **Non-deterministic and non-regenerable.** The same prompt gives a different
  tree tomorrow. You cannot version-control the intent, only the 40 MB result.
- Network latency (30 s – 3 min) and per-asset cost make it unusable as a
  runtime call. Requires G5 (an HTTP dependency) to even attempt in-process.
- No LODs, no alpha-cutout foliage convention, no submesh material split — all
  things your scatter/vegetation pipeline expects.

For it: it is genuinely the only way to get a *specific* hero prop you can't
parameterize (a particular statue, a particular vehicle). Your `.glb`/`.fbx`
parsers already exist and work.

**So: fold it in as one generator among many, run offline with a conditioning
pass.** See §3.5.

### Option B — AI-controlled procedural builders

**Verdict: this is the right core — with one important correction.**

It fits glGen natively: your entire existing content story is already
procedural and data-driven (terrain noise → chunk mesher → biome weights →
scatter manifest). Procedural output is deterministic, ~1 KB instead of 40 MB,
regenerable, parameterizable for LOD, and drops straight into
`createMeshFromData()`.

The correction: **do not have the AI write the geometry code.** LLM-authored
Lua/C++ that emits vertex arrays is where this approach usually dies — you get
inverted normals, broken UVs, degenerate triangles, and a debugging loop that
costs more than modelling would have. The reliable factoring is:

> **A human (you, once) writes the generators in C++. The AI writes
> *parameters* against a published schema.**

LLMs are excellent at filling a validated JSON schema and poor at emitting
correct mesh topology. This also removes the arbitrary-code-execution surface
entirely.

### Option C — MCP bridge

**Verdict: essential, but it's the *transport*, not the *strategy* — and it
must come third, not first.**

Blender-MCP works because Blender ships a huge Python API for the agent to
drive. glGen currently exposes ~15 Lua functions, none asset-related (G3).
Building MCP first hands the AI a remote control with no buttons.

But once there *are* buttons, MCP is what makes the system actually converge:
the generate → render → **look at the PNG** → critique → adjust-params loop is
the difference between "plausible parameters" and "a tree that looks right".
Your `requestCapture()` already produces the image; MCP returns it as an image
content block and the model genuinely sees it.

### Option D — Recommendation: **Recipe + Generator Registry, with an MCP control plane**

A hybrid whose centre of gravity is B, whose control plane is C, and where A is
demoted to one generator among many.

**The core idea: the recipe *is* the asset. The mesh is a build product.**

```json
{
  "id": "tree/oak_windswept_01",
  "generator": "tree.v1",
  "seed": 91733,
  "params": {
    "height": 8.4, "trunkRadius": 0.42, "trunkFlare": 0.35,
    "branchLevels": 4, "branchAngleDeg": 47, "branchDecay": 0.68,
    "lean": 0.25, "leanDirDeg": 210,
    "canopy": "broadleaf_cards", "canopyDensity": 0.8, "leafSize": 0.34
  },
  "material": {
    "bark":    { "generator": "tex.bark.v1",   "params": { "ridgeScale": 12, "tint": [0.32,0.26,0.20] } },
    "foliage": { "generator": "tex.leaf.v1",   "params": { "hue": 0.28, "autumn": 0.15 } }
  },
  "lodBias": 0,
  "provenance": { "prompt": "gnarled windswept oak on a ridge", "author": "claude", "date": "2026-08-01" }
}
```

Why this specific shape is the right one for glGen:

1. **Version-controllable intent.** A 1 KB diffable JSON file, not a binary
   blob. You can review what the AI decided, and `git blame` it.
2. **Deterministic and regenerable.** Same recipe + seed → byte-identical mesh,
   forever. Critical when the AI is the author and you need reproducible builds.
3. **The AI's job becomes schema-filling**, which is its strength. Every field
   is validated and clamped engine-side.
4. **One source of truth for two consumers.** Each generator publishes a JSON
   Schema. That schema *is* the engine's validator **and** the MCP tool
   description. They cannot drift.
5. **Option A slots in cleanly** as `{"generator": "import.gltf", "params": {"path": "..."}}`
   — imported assets and procedural assets become the same kind of thing.
6. **Variety for free.** One recipe + different seeds → an entire forest of
   distinct trees. Your scatter system already layers per-instance scale/lean/
   yaw/tint jitter on top (`ScatterLayer`), so the combinatorics are large.
7. **Instant iteration.** Edit the JSON → hot reload → `updateMeshFromData()`
   → the tree changes on screen without a restart.

---

## 3. Architecture

```
  ┌──────────────────────────────────────────────────────────────────┐
  │  Claude / any MCP client                                         │
  └────────────────────────────┬─────────────────────────────────────┘
                     MCP (stdio) │  tools + image results
  ┌────────────────────────────▼─────────────────────────────────────┐
  │  Tools/glgen-mcp   (Python — outside the C++ build)              │
  │  tool schemas generated from the engine's generator schemas      │
  └────────────────────────────┬─────────────────────────────────────┘
              JSON-RPC 2.0 over │ 127.0.0.1 TCP, newline-delimited
  ┌────────────────────────────▼─────────────────────────────────────┐
  │  glGenVk                                                          │
  │  ┌────────────────────────────────────────────────────────────┐  │
  │  │ Engine/Bridge/CommandServer   worker thread → main-thread Q │  │
  │  └───────────────────────────┬────────────────────────────────┘  │
  │  ┌───────────────────────────▼────────────────────────────────┐  │
  │  │ Engine/Generators/                                          │  │
  │  │   GeneratorRegistry   name → {fn, jsonSchema, version}      │  │
  │  │   AssetRecipe         parse / hash / serialize              │  │
  │  │   MeshBuilder         primitive kit generators are built on │  │
  │  │   TextureGen          noise → MeshImage (albedo/nrm/rough)  │  │
  │  │   gen/  tree.v1  rock.v1  grass.v1  kitbash.v1  import.gltf │  │
  │  └───────────────────────────┬────────────────────────────────┘  │
  │                    MeshData  │                                    │
  │  ┌───────────────────────────▼────────────────────────────────┐  │
  │  │ AssetManager::registerMeshData(assetId, MeshData)   ← NEW   │  │
  │  └───────────────────────────┬────────────────────────────────┘  │
  │  ┌───────────────────────────▼────────────────────────────────┐  │
  │  │ VulkanRenderSystem → createMeshFromData / updateMeshFromData│  │
  │  │ VulkanRenderer      bindless textures, BLAS, RT shadows     │  │
  │  └────────────────────────────────────────────────────────────┘  │
  └──────────────────────────────────────────────────────────────────┘
```

### 3.1 Why an external MCP process + an in-engine command port

Rather than embedding an MCP server in C++:

- Keeps MCP SDK/protocol churn out of your CMake build entirely.
- The command port is independently useful — CI, scripts, and your existing
  headless harness can all drive it.
- Tool descriptions can be iterated without recompiling the engine.
- One auth/sandbox boundary, bound to `127.0.0.1`, off by default.

### 3.2 Threading rule (get this wrong and you get random crashes)

`Registry`, `AssetManager`, `Scene` and `VulkanRenderer` are all single-threaded
with no locking. Therefore:

- The socket thread **only** parses JSON and pushes to a mutex-guarded queue.
- `main.cpp` drains the queue **once per frame at a fixed point** — after
  `renderSystem.update(reg, renderer)` and `terrain->addFrameInstances()`,
  before `renderer.drawFrame()`. Everything mutating runs there.
- `render.capture` is inherently **async**: the command enqueues
  `requestCapture()`, and the RPC reply is only sent on the *following* frame
  once the PNG is on disk. Design the reply path for deferred completion from
  the start.

### 3.3 Poly / BLAS budget (a real constraint in this renderer)

Every unique mesh costs a BLAS, and every placement costs a TLAS instance.
`ScatterLayer::castRayShadow=false` exists precisely because grass would
otherwise destroy the TLAS build. So the rule for AI-generated content:

> **A handful of recipes, many instances.** Get variety from per-instance
> transform/tint jitter (already implemented) and from a small number of
> seeds — never from a unique mesh per placement.

Enforce it with per-class poly budgets in the generator validators:
grass clump ≤ 200 tris, tree LOD0 ≤ 4k, rock ≤ 1.5k, hero prop ≤ 20k.

### 3.4 The honest risk: art direction, not plumbing

A dev-authored AI pipeline reliably produces *technically valid, visually
generic* output. Two things in your engine specifically mitigate this:

1. **You already have a strong unifying style layer.** `paintMaterial.glsl`,
   the outline pass, split-toning, the painterly terrain params — a consistent
   stylised look forgives a great deal of asset weakness that a photoreal
   target would expose. Lean on it hard.
2. **Fixed-framing turntable renders.** Critique only converges if the AI sees
   the asset the same way every time. Ship a `render.turntable` command with
   fixed light, fixed camera distance, a neutral backdrop, and a **1.8 m
   reference figure in frame** so scale errors are visible rather than
   inferred.

### 3.5 Where Option A lives

`Tools/glgen-fetch/` — a standalone Python CLI, **not** in the engine:

1. Call the text-to-3D service, poll, download the GLB.
2. **Conditioning pass** (the part that matters): decimate to the class poly
   budget, normalise scale to metres, fix up-axis, recenter pivot to base,
   split foliage into an alpha-cutout submesh, bake to the engine's material
   convention.
3. Write `assets/imported/<slug>.glb` **plus** a recipe
   `{"generator":"import.gltf","params":{"path":"assets/imported/<slug>.glb"}}`.

Offline because of latency, cost, non-determinism, and because a human or agent
should review before it enters the repo. **Do not add an HTTP client to the
engine** (G5 stays a gap on purpose).

---

## 4. Roadmap

### Phase 0 — Unblock the asset layer ✅ DONE

Closed G1, G2, G6.

`Engine/Assets/AssetManager.h/.cpp` — new synthetic-asset API:

```cpp
OBJHandle registerMeshData(const std::string &assetId,
                           std::unique_ptr<MeshData> data);
bool      replaceMeshData(const std::string &assetId,
                          std::unique_ptr<MeshData> data);
OBJHandle findMeshData(const std::string &assetId) const;   // lookup only, no disk
uint32_t  assetContentVersion(const std::string &assetId) const;
```

**Design note — two counters, not one.** The change counter is
`OBJRecord::contentVersion`, deliberately separate from
`AssetHandle::generation`. `generation` tracks handle *identity* (slot reuse,
which *must* invalidate stale handles); `contentVersion` tracks *content*
(same asset, new bytes — handles stay valid). Folding regeneration into
`generation` would have made every `MeshComponent::objHandle` already pointing
at the asset resolve to null, i.e. the mesh would vanish on regeneration. The
plan originally called this `assetGeneration`; renamed to remove the
ambiguity.

Also landed:

- `registerMeshData` **keeps the CPU `MeshData`** (the removed
  `registerRuntimeOBJRaw` stored only a GPU model and dropped it, which is why
  it could never have worked for this). Its slot-recycling predicate tests
  CPU *and* GPU data — the old gpu-only test would hand out a live synthetic
  asset's slot.
- Deleted `Engine/Assets/PrimitiveMeshGenerator.h` (dead; included the deleted
  `OBJModel.h`) and `registerRuntimeOBJ`/`registerRuntimeOBJRaw`.
- `VulkanRenderSystem::mMeshByAsset` now caches `{handle, contentVersion}` and
  calls `VulkanRenderer::updateMeshFromData()` on divergence;
  `resolveMeshData()` gained a `gen://` branch that never falls through to the
  filesystem.
- **Two latent bugs fixed** by the same mechanism: `recenterOBJ`/`rotateOBJ`
  and `pollHotReload` all mutate CPU geometry in place and now bump
  `contentVersion`. Previously they only worked because every caller happened
  to run before the first frame — editing a watched `.obj` on disk did not
  actually update the rendered mesh.

**Known limitation:** `assetContentVersion` covers OBJ/synthetic records only.
glTF/FBX assets report 0 ("never stale"). Generated content is all
OBJ-record-backed, so this is a gap for imported models, not for the pipeline.

**Verification:**

- `Tests/test_asset_manager.cpp` — 9 cases covering register/replace/find,
  handle survival across replacement, version semantics, slot recycling, and
  coexistence with `__primitive_*` ids. Suite: **118/118 pass**.
- `GLGEN_GEN_TEST=<frame>` in `runtime/main.cpp` builds a `MeshData` in memory
  (`makeGeneratedBox`), registers it as `gen://phase0/box`, spawns an entity,
  and regenerates it at the given frame. Run headless:

  ```
  GLGEN_GEN_TEST=6 GLGEN_SMOKE_FRAMES=12 GLGEN_SMOKE_CAPTURE=captures/gen_phase0.png
  ```

  Writes `…before.png` (short wide red box) and `…png` (tall narrow green box)
  — same entity, same asset id, geometry hot-swapped with no restart and no
  Vulkan validation errors.

### Phase 1 — Generator framework + first generators ✅ DONE

Everything below shipped under `Engine/Generators/`. Suite: **157/157 pass**
(+39 cases). Design notes and discoveries first, then the file map.

**Named vs anonymous recipe ids.** `assetIdFor()` addresses the two cases
differently, and both halves are load-bearing:

| Recipe | Asset id | Why |
|---|---|---|
| named (`"id": "tree/oak"`) | `gen://tree.v1/tree/oak` | **stable** — editing the recipe keeps the id, so already-placed entities pick up the new geometry. Content-addressing here would mint a new asset on every save and silently strand every tree in the scene |
| anonymous | `gen://tree.v1/<hash>` | content-addressed — an AI asking for "a rock with these params" has no identity to preserve, so repeat requests deduplicate automatically |

**Two renderer gaps found, both worked around rather than expanded into:**

1. **No normal-map binding in `mesh.frag`.** It samples diffuse/roughness/
   metallic/AO only; normal maps exist for terrain material slots but not for
   meshes. `TextureGen` therefore does not generate them — it would burn
   memory and a bindless slot on data nothing reads.
2. **No alpha cutout in the mesh pipeline.** `DrawItem` has no alpha-cutoff
   field and `materialFlags` has no alpha-test bit, so
   `MaterialAsset::alphaCutoff` is silently ignored — an alpha-masked foliage
   quad renders as an **opaque rectangle** (confirmed visually before the
   fix). Foliage silhouettes therefore live in *geometry*:
   `MeshBuilder::addLeafCard()` is a 6-vertex/4-triangle pointed oval, and
   grass blades taper geometrically. Cheaper than adding a cutout path to the
   renderer, and a better fit for the engine's faceted look. The alpha channel
   is still written, so it starts working the day `mesh.frag` gains cutout.

**Files**

| File | Role |
|---|---|
| `GenRandom.h` | xorshift32 + open-coded distributions. `std::uniform_real_distribution`'s mapping is **not** standardized across libstdc++/libc++/MSVC, which would break the "same recipe, same mesh, every machine" premise |
| `MeshBuilder.h/.cpp` | box, tapered cylinder, icosphere, quad, card, leaf card, revolve; transform stack, displace, smooth/flat normals. Emits **indexed** geometry |
| `GeneratorSchema.h/.cpp` | `SchemaBuilder` + validation: defaults filled, numbers **clamped with a warning**, wrong types and out-of-enum values **rejected**. Clamping keeps an AI productive (usable asset + text describing the adjustment); enum substitution would silently change what the asset *is* |
| `GeneratorRegistry.h/.cpp` | name → {schema, poly budget, build fn}; validates before any geometry code runs |
| `AssetRecipe.h/.cpp` | parse/serialize/hash. Hash covers generator+seed+params+material; **excludes** `id` and `provenance` so renaming or annotating never invalidates a cached mesh |
| `TextureGen.h/.cpp` | `tex.bark`, `tex.rock`, `tex.leaf`, `tex.grass`, `tex.wood`, `tex.metal` → `MeshData::images`. Seamless in U via circular noise sampling, so anything wrapped around a trunk has no seam |
| `AssetLibrary.h/.cpp` | recipe → generator → textures → `registerMeshData`; caching, `loadAll`, file-watching `pollHotReload` |
| `gen/Gen{Tree,Rock,Grass,Kitbash,Import}.cpp` | the five generators |

**Generators shipped:** `tree.v1` (conifer/broadleaf/blob/bare, two submeshes,
golden-angle branch phyllotaxis), `rock.v1` (icosphere + fBm + planar cuts,
sits on y=0), `grass.v1` (≤200 tris), `kitbash.v1` (parts list, grouped into
one submesh per colour — deliberately **not** CSG: booleans need robust
predicates and produce degenerate topology at coincident faces, which would be
the single biggest source of broken output), `import.gltf` (Option A's landing
point; loads from disk only, no network).

**Verification:** `Tests/test_generators.cpp` — determinism (bitwise identical
across runs, different across seeds), schema clamp/reject behaviour, recipe
hashing, per-generator invariants, texture payload/cutout, library caching,
in-place regeneration, hot reload via a real temp directory, and one-bad-file
resilience.

`GLGEN_GEN_SHOWCASE=<filter>` lines recipes up **against open sky at altitude**
with fixed camera and light — a minimal version of §3.4's turntable. The
altitude is the point: at ground level the streamed forest occludes exactly the
silhouette you need to read. It found the sparse-canopy and opaque-rectangle
defects that ground-level captures had hidden.

```
GLGEN_GEN_SHOWCASE=1 GLGEN_SMOKE_FRAMES=10 GLGEN_SMOKE_CAPTURE=captures/gen_row.png
```

### Phase 1 — original plan (superseded by the section above)

New `Engine/Generators/`:

**`MeshBuilder.h/.cpp`** — the kit every generator is written against. This is
what makes a generator 150 lines instead of 800:
`addBox`, `addTaperedCylinder(p0,r0,p1,r1,segments)`, `addIcosphere(subdiv)`,
`addQuad`, `addCard(w,h,bendDeg)`, `revolve(profile,segments)`,
`extrudeAlong(path,profile)`, `appendTransformed(other, mat4)`,
`beginSubmesh(name, MaterialAsset)`, `computeSmoothNormals(angleThreshold)`,
`weldVertices(eps)`, `computeBounds()`, `triangleCount()`.
Emits **indexed** geometry (fixing G6's non-indexed wart).

**`GeneratorRegistry.h/.cpp`**

```cpp
struct GeneratorInfo {
  std::string name;         // "tree.v1"
  std::string description;  // becomes the MCP tool description
  nlohmann::json schema;    // JSON Schema: types, ranges, defaults, enums
  int polyBudget = 0;
  std::function<std::unique_ptr<MeshData>(const nlohmann::json &params,
                                          uint32_t seed,
                                          std::vector<std::string> &warnings,
                                          std::string &error)> build;
};
class GeneratorRegistry {
public:
  static GeneratorRegistry &instance();
  void registerGenerator(GeneratorInfo info);
  const GeneratorInfo *find(const std::string &name) const;
  std::vector<std::string> names() const;
  // Validates params against the schema, clamps out-of-range values into
  // `warnings`, and returns false with `error` set on anything unrecoverable.
  bool validate(const std::string &gen, nlohmann::json &params,
                std::vector<std::string> &warnings, std::string &error) const;
};
```

**`AssetRecipe.h/.cpp`** — parse/serialize/hash. `assetIdFor(recipe)` returns
`"gen://" + generator + "/" + sha1(canonical_json)`.

**`AssetLibrary.h/.cpp`** — resolves a recipe: cache lookup → run generator →
run material generators → attach `MeshImage`s → `registerMeshData()`. Watches
`assets/recipes/*.json` and calls `replaceMeshData()` on change (hot reload).

**`TextureGen.h/.cpp`** — procedural PBR maps straight into `MeshImage`
(reusing `Core/PerlinNoise.h`). Albedo + normal (from height derivative) +
roughness at 512²/1024². Generators: `tex.bark`, `tex.leaf`, `tex.rock`,
`tex.grass`, `tex.wood`, `tex.dirt`, `tex.metal`. **Needs zero renderer
changes** — the payloads flow through `MeshData::images` into the bindless
array via the path verified at `VulkanRenderer.cpp:2965`.

**Ship these generators first** (chosen to match what the engine already
scatters and what a solo dev actually needs):

| Generator | Covers | Notes |
|---|---|---|
| `tree.v1` | conifers, broadleaf, saplings, dead snags | recursive branching; 2 submeshes (bark / alpha-cutout foliage); `lodBias` param |
| `rock.v1` | boulders, scree, cliff chunks | icosphere + fBm displacement + planar cuts |
| `grass.v1` | clumps, tufts, ferns, reeds | curved cards; must stay ≤ 200 tris |
| `kitbash.v1` | crates, fences, posts, barrels, doors, simple buildings | box/cylinder assembly from a parts list — **highest leverage for generic props** |
| `import.gltf` | anything Option A or a store produced | wraps the existing parsers |

**Done when:** editing `assets/recipes/oak.json` changes the tree on screen
without a restart, and the trees come from `tex.bark`-generated textures.

### Phase 2 — Scripting & authoring surface ✅ DONE

Closed G3 and G4. Suite: **169/169 pass** (+12).

**Layering: `render.*`/`terrain.*` cannot live in EngineCore.** They need
`VulkanRenderer` and `VkTerrainSubsystem`, which sit *above* EngineCore.
`ScriptSystem` therefore grew a hook:

```cpp
using BindingHook = std::function<void(sol::state &)>;
void addBindingHook(BindingHook hook);   // runs during initialize()
```

`VkScriptSubsystem` installs `registerVkScriptBindings()` through it, so
EngineCore never acquires a dependency on its own consumer.

**`io` and `os` are no longer opened.** `ScriptSystem::initialize()` used to
open both. With scripts increasingly machine-authored, `os.execute` alone turns
the Lua console into a remote shell. Nothing in `scripts/` used either library.

**API**

| Table | Functions |
|---|---|
| `assets` | `generators()`, `texture_generators()`, `schema(name)`, `define{...}`, `list()`, `asset_id(recipeId)`, `reload()` |
| `world` | `spawn(assetId, opts)` (+ the pre-existing spawners) |
| entity | `set_material{...}`, `set_asset(id)` (+ existing transform/physics methods) |
| `scene` | `stats()`, `save(path)`, `load(path)` |
| `render` | `params{...}`, `get_params()`, `capture(path)`, `stats()` |
| `terrain` | `height_at(x,z)`, `regenerate{...}`, `settings()`, `scatter_layer{...}`, `scatter_layers()` |

`assets.define` returns `(assetId, warnings)` on success and `(nil, error)` on
failure. Warnings are **both** returned and logged — a script may ignore the
second return value, and a silently clamped parameter is how an asset quietly
stops matching its recipe.

**Two entry points, one VM:** an editor **Console prompt** (Console tab,
bottom) and **`GLGEN_SCRIPT=<file>`** for headless runs. Both go through
`ScriptSystem::execString()`, which is also what the Phase 3 command port will
call.

**`ScriptJson.h`** carries the Lua↔JSON conversion, deliberately free of
GLFW/ECS includes so it is directly testable. Two asymmetries are forced by
Lua and are documented in the header and pinned by tests:

- Lua has **one table type**. A table whose keys are exactly `1..n` becomes a
  JSON array; anything else an object. Sparse integer keys (`{[1]=a,[5]=b}`)
  become an **object** — treating them as an array would silently drop
  entries. An empty table is genuinely ambiguous and becomes an object.
- Lua has **one number type**. Integer-valued doubles are written as JSON
  integers, or `3.0` lands in `recipeHash()` where `3` belongs and asset
  identity drifts.

**Verification:** `Tests/test_script_json.cpp` (12 cases, including a
params-table round trip asserted with `==` because that value feeds
`recipeHash()`), plus `scripts/ai_asset_demo.lua` run headlessly — it
discovers generators, reads `tree.v1`'s schema, defines a textured birch,
demonstrates parameter clamping surfacing back to Lua, clears the default
scatter via `terrain.regenerate`, spawns the birch plus a six-rock ring of
per-seed variants, and frames a capture.

```bash
GLGEN_SCRIPT=scripts/ai_asset_demo.lua GLGEN_SMOKE_FRAMES=16 GLGEN_SMOKE_CAPTURE=captures/lua_demo.png
```

Building the Lua VM into the test target required enabling **C** in
`Tests/CMakeLists.txt` (`project(... LANGUAGES C CXX)`) — it declared `CXX`
only, and CMake cannot determine a linker language for a C-only target.

### Phase 2 — original plan (superseded by the section above)

Closes G3, G4. `Engine/Scripting/ScriptBindings.h`:

```lua
-- assets
assets.define{ id="tree/oak", generator="tree.v1", seed=91733, params={...} }  --> assetId
assets.regenerate(assetId, { height = 9.2 })
assets.generators()            --> { "tree.v1", "rock.v1", ... }
assets.schema("tree.v1")       --> table

-- world
world.spawn(assetId, { pos=Vec3(0,0,0), rot=Vec3(0,45,0), scale=1.2, collider="capsule" })
entity:set_material{ baseColor={0.4,0.3,0.2}, roughness=0.8, emissive=0.0 }

-- scene / terrain / render
scene.save(path); scene.clear(); scene.stats()
terrain.regenerate{ seed=7, heightScale=24 }
terrain.scatter_layer{ name="oaks", meshPath=assetId, density=0.008, biomeForest=2.0 }
render.params{ exposure=0.9, sunPitch=35, timeOfDay=false }
render.capture("captures/oak.png")
```

`ScriptSystem`: add `bool execString(const std::string &src, std::string &err)`
and an editor Console input line. **Sandbox note:** `initialize()` currently
opens `sol::lib::io` and `sol::lib::os` — the agent-facing environment must not
inherit those.

### Phase 3 — Command port ✅ DONE

Closed G7. Suite: **178/178 pass** (+9), plus a live end-to-end smoke script.

**Split, mirroring ScriptBindings/VkScriptBindings:**

| File | Role |
|---|---|
| `Engine/Bridge/CommandServer.h/.cpp` | sockets, framing, queues, deferred replies. Engine-agnostic |
| `VulkanRHI/runtime/VkAgentBridge.h/.cpp` | the handlers, which need `VkAppState` + renderer + ECS |
| `Tools/glgen-client/glgen_client.py` | reference client; Phase 4's MCP server wraps it |
| `Tools/glgen-client/smoke.py` | end-to-end check, exits non-zero on failure |

**Deliberately few handlers.** Phase 2 already bound the whole authoring API to
Lua, so `script.eval` covers everything not listed rather than the port growing
a second, parallel implementation that can drift. A method is typed only when
an MCP client wants structured parameters, or when it *cannot* answer
synchronously: `engine.info`, `script.eval`, `assets.generators`,
`assets.schema`, `assets.define`, `scene.spawn`, `scene.query`, `scene.clear`,
`render.setParams`, `render.getParams`, `render.capture`, `render.turntable`.

Render params were refactored so the Lua binding and `render.setParams` share
**one** JSON-canonical mapping (`applyRenderParamsJson`) — the Lua side now
converts its table via `ScriptJson` and calls the same function.

**The threading rule, enforced and tested.** Socket thread only accepts, reads,
frames and parses; `poll()` runs handlers on the main thread at one fixed point
(after gameplay/physics settle, before `renderSystem.update()`, so an entity a
handler spawns is drawn *this* frame). A test asserts the handler's
`std::this_thread::get_id()` equals the polling thread's.

**Deferred replies** are why the protocol carries ids. `render.capture`
completes in `postFrame()` — after the `drawFrame()` that wrote the PNG, the
first honest moment it exists. `render.turntable` is a per-frame state machine
(N shots need N draws), which frames the asset from its own bounds, lifts it
clear of the terrain, pins sun and exposure so two turntables are comparable,
and restores the camera afterwards.

**Three bugs the tests found that the Phase 2 demo could not:**

1. **`sol::as_table` is load-bearing.** Returning a bare `std::vector` from a
   binding makes sol push a *container userdata*, not a Lua table. It indexes
   and concatenates like one — so `ai_asset_demo.lua` passed — but `ScriptJson`
   saw opaque userdata and `assets.generators()` came back **null over RPC**.
2. **Windows `SO_REUSEADDR` is not POSIX `SO_REUSEADDR`.** It lets a second
   process bind an address already in use, so any process could have hijacked
   a port that runs arbitrary Lua in the engine. Windows now uses
   `SO_EXCLUSIVEADDRUSE`; POSIX keeps `SO_REUSEADDR`, where it only skips
   TIME_WAIT. A test binds the same port twice and requires the second to fail.
3. **Parse errors were swallowed.** The "null id ⇒ notification ⇒ no reply"
   rule is right for notifications but left a client waiting forever on a JSON
   typo. Parse errors now reply with a null id, per JSON-RPC.

**Security:** loopback-only bind, off unless `GLGEN_AGENT_PORT` is set. Anyone
who can connect can run arbitrary Lua in the process — a development tool, not
something to expose.

```bash
GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe
python Tools/glgen-client/smoke.py
```

### Phase 3 — original plan (superseded by the section above)

Closes G7. `Engine/Bridge/CommandServer.h/.cpp` — ~250 lines, `ws2_32` on
Windows behind a small platform shim. Off by default; `--agent-port N` /
`GLGEN_AGENT_PORT`. Bound to `127.0.0.1` only.

Methods: `engine.info`, `scene.query`, `scene.spawn`, `scene.delete`,
`entity.transform`, `assets.generators`, `assets.schema`, `assets.define`,
`assets.regenerate`, `assets.validate`, `script.eval`, `render.setParams`,
`render.capture`, **`render.turntable`**, `editor.save`, `editor.load`.

Wire the drain call into `main.cpp` at the point described in §3.2.

### Phase 4 — MCP server ✅ DONE

`Tools/glgen-mcp/server.py` + `README.md`. Verified by `--selftest` (all
checks pass), by a real stdio session, and by running the critique loop.

**Two deviations from the original plan, both forced by what is installed:**

1. **No MCP SDK** (`mcp` is not installed). MCP's stdio transport is
   newline-delimited JSON-RPC 2.0 — the same protocol the command port already
   speaks — and the needed surface is `initialize` / `tools/list` /
   `tools/call`. Standard library only, so `python server.py` runs with no
   install step. Documented as the thing to revisit if resources/prompts/
   sampling are ever needed.
2. **No Pillow**, so images could not be downscaled in Python. Fixed at the
   source instead: `VulkanRenderer::requestCapture(path, maxDimension)` box-
   downscales before writing. A native capture is ~1.4 MB → ~1.9 MB base64,
   unusable at several frames per render; at 768 px it is ~400 KB. Better
   placed there anyway — no Python image dependency.

**Tools are generated from the engine's schemas.** On connect the server reads
`assets.generators` + `assets.schema` and emits one `glgen_create_<generator>`
tool whose `inputSchema` **is** that generator's validation schema. `tree.v1`
surfaces 21 parameters with real bounds and enums, so the model reads
`height: 0.3 … 40` before its first call instead of guessing. This is the
payoff of §2 point 4 — one document, two consumers, no drift.

**Renders hide the editor UI** (`VkAgentBridge::wantsCleanFrame()`, honoured in
`main.cpp`). Docked ImGui panels cover roughly a third of the frame and are
pure noise when the question is "does this asset look right".

**The loop demonstrably converges.** Starting from deliberately poor
parameters:

| Iteration | Result |
|---|---|
| `branchAngleDeg 80, canopyDensity 0.35, leafSize 0.3` | 400 tris — image shows a clothes-line: near-horizontal branches, almost no foliage |
| corrections read off the image (`42°`, density `0.85`, taper `0.55`) | recognisable broadleaf tree — but **text** warns 5520 tris, over the 4000 budget |
| `branchesPerLevel 3, radialSegments 5, density 0.7` | 2338 tris, no warnings, tree intact |

Both feedback channels earn their place: the **image** carries proportion and
silhouette, the **text** carries what an image cannot — clamped parameters,
poly budgets, unmatched material slots.

### Phase 4 — original plan (superseded by the section above)

`Tools/glgen-mcp/` (Python). stdio to the client, TCP to the engine. Tool
schemas are **generated at connect time from `assets.schema`** so they never
drift from the engine. `render_asset` / `render_turntable` return MCP **image**
content blocks so the model actually sees the result. Auto-launches `glGenVk`
via the existing `run-glgenvk.bat` if the port isn't answering.

This is where the loop closes:

```
define recipe → render_turntable → SEE IT → "canopy too sparse, trunk too straight"
             → regenerate{canopyDensity:+0.2, lean:0.3} → render again → accept
```

### Phase 5 — Text-to-3D ingestion ✅ DONE (with one part unverified)

Suite: **188/188 pass** (+10 decimation cases).

**Scope call: the conditioning is the substance, the fetch is 40 lines of
HTTP.** An unconditioned 234k-triangle download is not a smaller version of a
good asset — it is an unusable one, because this renderer builds one
acceleration structure per unique mesh. So the work went into conditioning,
and it lives **in the engine**, which already has the `.obj`/`.gltf`/`.glb`/
`.fbx` parsers and now the decimator. `Tools/glgen-fetch/fetch.py` drives it
over the command port and writes the recipe.

**New: `Engine/Generators/MeshDecimate.{h,cpp}`** — quadric error metric
(Garland & Heckbert) edge collapse. Chosen over vertex clustering, which is far
simpler but bulldozes silhouettes, and silhouette is exactly what survives at
the distance scattered props are seen from. Notable details:

- **welds coincident positions first** — imported meshes split vertices at UV
  seams, which leaves the topology shredded and stops collapses dead;
- **refuses collapses that flip a face normal**, which is what stops the
  surface folding through itself;
- **budgets submeshes proportionally**, so a small detail part (a hinge, a
  handle) is not erased to save a large one;
- **never returns an empty mesh** — handing the renderer zero geometry is worse
  than ignoring the request;
- **deterministic**, or an import would break the recipe reproducibility
  promise.

**`import.gltf` gained** `maxTriangles`, `normalizeHeight` and `normalizeSize`.

**Two bugs found by running it on a real 7 MB GLB:**

1. **`normalizeHeight` scaled the thin axis.** A wrench lies flat — 0.085 units
   tall, 1.0 long — so forcing its *height* to 1 m made it ~8 m long. Added
   `normalizeSize` (longest axis), made it the default for `rock`/`prop`/`hero`
   where orientation is unknown, and made the height path *warn* when the model
   is more than 3× longer than it is tall.
2. **The turntable's orbit radius had a 2 m floor**, which framed a boulder
   well and left a hand-sized prop a few pixels wide. The floor now exists only
   to clear the 0.05 m near plane.

**Decimation quality, checked directly:** the same asset imported raw (233,974
tris) and decimated (8,000 tris) renders **visually identically** at review
distance — a 29:1 reduction with no silhouette loss.

**Result:** an imported asset is now the same kind of thing as a generated one.
`assets/recipes/imported_wrench.json` loads at startup beside the five
procedural recipes, hot-reloads on edit, and can be used in a scatter layer.

```bash
python Tools/glgen-fetch/fetch.py --from-file model.glb --slug oak --class tree --render
```

**⚠ The provider adapters are UNVERIFIED.** Text-to-3D services need a paid
account, and nothing here has been run against a live API. `fetch_generic()`
implements the submit → poll → download shape those services share, and the
`meshy`/`tripo` entries point at their documented endpoints, but the field
names are **not** a standard and yours will differ. Treat an adapter as a
starting point to check against current docs, not as known-good code. The
`--from-file` path is what is actually tested, and it is also the path that
matters if you already have the file.

### Phase 5 — original plan (superseded by the section above)

`Tools/glgen-fetch/` per §3.5. Offline CLI, conditioning pass, emits a GLB plus
an `import.gltf` recipe.

### Phase 6 — Guardrails & regression ✅ DONE

Suite: **202/202 pass** (+14 validation cases), plus a working regression gate.

**Validators — `Engine/Generators/MeshValidate.{h,cpp}`**, wired into
`GeneratorRegistry::run()` so *every* path (recipes, Lua, command port, MCP)
inherits them and a new generator cannot forget. Checks: non-finite data,
out-of-range indices, degenerate triangles, zero-length normals, poly budget,
pivot placement, off-origin authoring, absurd/vanishing extent, missing UVs on
a textured submesh. All warnings except non-finite data, which is refused
outright — it corrupts the acceleration-structure build and crashes far from
the cause.

**Pointing the validators at the existing generators found a real bug.**
`grass.v1` was emitting **25% degenerate triangles**: `MeshBuilder::addCard`
tapers to a point, so each blade's top quad had two coincident vertices and one
zero-area triangle. On the most-instanced asset in the engine. Fixed by
emitting a single triangle for a band that ends in a point — grass went from
28 to **21 triangles per clump** with no visual change.

**Two checks were wrong and were changed on evidence, not taste:**

- **Non-manifold edges: removed entirely.** It fired on `tree.v1` (73 edges) —
  correctly, since a tree is overlapping cylinders plus loose foliage cards —
  and would fire on most real game assets, which are full of double-sided
  planes. A warning that cries wolf doesn't just waste a line; it teaches the
  reader to skim past the ones that matter.
- **Off-origin threshold relaxed** from a small tolerance to half the object's
  own size. It flagged `tree.v1` for a crown 1.27 m off-centre over its trunk,
  which is what crowns do. What the check is actually for is geometry authored
  *tens of metres* from its origin.

**Regression — `Tools/glgen-regress/`**, two complementary signals:

| Signal | Catches | Blind to |
|---|---|---|
| metrics (tris, verts, bounds) | generator changes, exactly and by name | anything about shading |
| golden images | shaders, materials, flipped normals | nothing visual, but noisier |

Demonstrated complementary: recolouring a recipe left **metrics passing**
(geometry untouched) while **both images failed** at ~7% of pixels.

**Making renders reproducible took three fixes**, each found by re-running an
unchanged check and watching it fail:

1. **The animation clock** — cloud drift, dew twinkle, volumetric turbulence.
   Added `VulkanRenderer::Params::fixedTimeSeconds`.
2. **The live world in frame** — at +55 m the streaming terrain, settling
   physics bodies and vegetation were all visible and all differed run to run.
   The turntable now lifts the subject **past the far plane**, so the world is
   not drawn at all. This also gives the clean sky backdrop §3.4 wanted.
3. **The camera grade**, which eases toward the camera's biome on the real
   clock. Disabled during a turntable.

An unchanged re-run now reports a mean per-pixel delta of **0.00**. Tolerance
is deliberately tight (4/255 per channel, 0.2% of pixels) — a loose tolerance
is how a regression check quietly stops working.

The PNG comparison uses stdlib `zlib` only; no image dependency.

```bash
python Tools/glgen-regress/regress.py            # check, non-zero exit on diff
python Tools/glgen-regress/regress.py --update   # accept intended changes
```

### Phase 6 — original plan (superseded by the section above)

- **Engine-side validators** returned as `warnings` in every RPC reply, so the
  AI gets *text* feedback alongside pixels: poly budget, degenerate triangles,
  non-manifold warning, bounds sanity, UV coverage, pivot-at-base check.
- **Golden-image regression**: extend the existing `GLGEN_SMOKE_CAPTURE`
  harness with a recipes smoke run that turntables every recipe into
  `captures/`. The machinery already exists — this is mostly a script.

---

## 5. Summary of the recommendation

| | |
|---|---|
| **Foundation** | Option B, corrected: AI writes **validated parameters**, not geometry code |
| **Unit of authorship** | The **recipe** (a ~1 KB versioned JSON doc). The mesh is a build product |
| **Control plane** | Option C (MCP), built **third** — after there is an API worth driving |
| **Text-to-3D** | Option A, demoted to one generator, run **offline** with a conditioning pass |
| **Blocker to clear first** | `AssetManager::registerMeshData()` — everything waits on it |
| **Biggest technical asset you already have** | `MeshData` + `createMeshFromData`/`updateMeshFromData` + embedded-`MeshImage` bindless upload |
| **Biggest real risk** | Art direction, not plumbing — mitigate with the painterly style layer and fixed-framing turntable critique |

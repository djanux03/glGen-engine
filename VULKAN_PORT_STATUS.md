# glGen — Vulkan Port: Status & Handoff

> Written before a machine format so nothing is lost. This captures where the
> OpenGL→Vulkan rewrite stands, how to build/run it, and exactly what's left.

## TL;DR

- **Goal:** a *fully* Vulkan engine with the **same functionality** as the old
  OpenGL engine (UI/editor, mouse-look/zoom, terrain generator, etc.) **plus**
  modern Vulkan features (ray tracing, mesh shaders, bindless, …). Not a 1:1
  port — reuse the API-agnostic systems, rewrite rendering modernly.
- **Branch layout:**
  - `main` — the **old OpenGL engine** (preserved, untouched).
  - `vulkan-engine` — **all the Vulkan work + integration** (this branch).
- **Where it is:** the engine-core/render split is DONE — `EngineCore` is a
  GL-free static library, and **`glGenVk`** is a real Vulkan runtime running
  the engine's actual Scene/AssetManager/Jolt-physics systems with per-frame
  dynamic RT (TLAS rebuilt every frame). The actual `glGen` game exe is still
  OpenGL; the Vulkan path is opt-in (`-DGLGEN_BUILD_VULKAN=ON`).

## Build & run

Prereqs: **Vulkan SDK** (LunarG; dev used 1.4.350.0 at `C:/VulkanSDK/<ver>`),
CMake, Visual Studio 2026 ("Visual Studio 18 2026" generator), an RTX/RT-capable
GPU (dev used an RTX 3070).

- **Vulkan renderer/demo** (Windows): run `run-vulkan.bat` from the repo root, or:
  ```
  cmake -S . -B Build-vs18 -G "Visual Studio 18 2026" -A x64 -DGLGEN_BUILD_VULKAN=ON
  cmake --build Build-vs18 --config Release --target glGenVulkanSmoke
  .\Build-vs18\bin\Release\glGenVulkanSmoke.exe
  ```
  The **real Vulkan runtime** (EngineCore + physics) is the `glGenVk` target:
  `cmake --build Build-vs18 --config Release --target glGenVk` then
  `.\Build-vs18\bin\Release\glGenVk.exe` (needs `Jolt.dll` from the same bin
  dir, already there).
  Controls: **RMB** = mouse-look, **scroll** = zoom (FOV), **WASD + Space/Ctrl**
  = fly (Shift = fast). ImGui panel: exposure, light (day/night), terrain
  generator (amplitude/frequency/octaves/seed), draw-terrain toggle.
  - `GLGEN_BUILD_VULKAN` defaults **OFF** so the OpenGL build is unaffected.
  - Env: `GLGEN_SMOKE_FRAMES=N` auto-exits after N frames; `GLGEN_SMOKE_CAPTURE=<path.png>`
    writes a PNG of a frame (headless verification).
- **Old OpenGL engine:** `build.ps1` / `build.sh` (unchanged; builds `glGen`).

## What the Vulkan renderer (`VulkanRHI/`) already does

Vulkan 1.3 core throughout (dynamic rendering + synchronization2), verified
runtime-clean with validation layers on:

- **Foundation:** instance+validation, physical-device selection requiring RT +
  acceleration-structure + mesh-shader, VMA allocator, swapchain, 2 frames in
  flight, PSO cache (disk-persisted).
- **Bindless** textures (partially-bound, update-after-bind array).
- **Per-frame UBO** ring (replaces immediate-mode uniforms).
- **Mesh draw path:** OBJ loader (tinyobjloader), per-material submeshes,
  stb_image sRGB textures, device-local vertex/index buffers via staging, depth.
- **HDR** offscreen target + **ACES tonemap**; correct sRGB/linear.
- **Hardware ray-traced shadows** (BLAS/TLAS + ray query `rayQueryEXT`) — replaced
  cascaded shadow maps entirely.
- **GPU-driven terrain** via **mesh shaders** (`VK_EXT_mesh_shader`): task shader
  frustum-culls patches, mesh shader generates a displaced grid. Live generator
  params. Slope/height materials (sand/grass/rock/snow).
- **Procedural sky** + day/night (sun elevation driven by the UI light).
- **Aerial/distance fog**, day/night aware.
- **Free-fly camera** (mouse-look/zoom/WASD).
- **Dear ImGui** UI on the Vulkan backend (imgui_impl_vulkan, dynamic rendering).
- **Multi-object scene:** meshes + instances with per-object transforms; TLAS
  over all instances.
- **Headless PNG capture** for verifying visuals.

### `VulkanRHI/` files
`VulkanContext` (instance/device/VMA), `VulkanSwapchain`, `VulkanBindless`,
`VulkanPipelineCache`, `VulkanMesh` (OBJ load), `VulkanAccel` (BLAS/TLAS),
`VulkanRenderer` (the renderer + engine-drivable API), `vk_mem_alloc_impl.cpp`,
`vk_stb_write_impl.cpp`, `smoke/main.cpp` (runtime/demo). Shaders in
`shaders/vulkan/` (mesh, terrain.task/mesh/frag, sky, tonemap).

## Engine integration — the actual "make it the engine" work

**Strategy:** strangler / core-render split. Reuse the API-agnostic ~2/3 of the
engine (ECS, Scene, physics/Jolt, audio, networking, Lua scripting, gameplay,
asset *parsing*, input, editor *logic*); replace only the rendering layer with
`VulkanRHI`; rebuild the render loop + FX on Vulkan; swap the window from a GL
context to a Vulkan surface; swap the editor's ImGui backend gl3→vulkan; port FX;
delete OpenGL last. **Do NOT re-implement ECS/physics/scripting — they're
API-agnostic.**

**OpenGL-coupling map (only these touch GL):** `Engine/Rendering/*`,
`Engine/Terrain/TerrainGpuRenderer`, `Engine/Assets/{OBJ,FBX,UFBX}Model.{h,cpp}`
(the GPU-upload half), `Engine/ECS/Systems/RenderSystem.h` (GL draw bridge), and
`PhysicsSystem.cpp`'s debug-draw method (sim itself is GL-free). Everything else
(ECS `Registry`/`Components`, `Scene`, `Core`, `Scripting`, `AssetManager`,
`Input`) is/was API-agnostic.

### Integration DONE (on `vulkan-engine`)
1. **Renderer is engine-drivable** — retained scene API: `createMeshFromObj(path)
   -> MeshHandle`, `addInstance(mesh, transform)`, `finalizeScene()` (builds
   BLAS/TLAS). init() takes no hardcoded scene.
2. **`MaterialAsset` decoupled from OpenGL** — pure data (`GLuint`→`uint32_t`;
   `apply(Shader&)` member → free `applyMaterial()` in the GL layer). This frees
   `Components.h`/ECS from GL. (Verified: `Engine.lib` still compiles.)
3. **The Vulkan scene is driven by the engine's real ECS** — `smoke/main.cpp`
   builds a `Registry` (`TransformComponent` + `MeshComponent(assetId=path)`),
   then a bridge resolves each entity's mesh → renderer handle and submits
   instances. **This bridge is the seed of the Vulkan render system.** The ECS
   is header-only + GL-free, so the Vulkan app links no engine code — just the
   include paths.
4. **`Input` decoupled from OpenGL** — `Keyboard.h`/`Mouse.h` only used GLFW; the
   dead `glad` include was removed.

5. **Asset parse/upload split (the keystone) — DONE.** Model loading is split
   into CPU parse (`MeshData` + `MeshParse{OBJ,GLTF,FBX}.cpp`, agnostic) and
   per-backend GPU upload (`loadFromData` on the GL model classes,
   `createMeshFromData` on the Vulkan renderer). `AssetManager` is GL-free:
   it owns parsed `MeshData` and delegates GPU model create/reload/destroy to
   a registered `ModelGpuBackend` (GL installs `InstallGLModelBackend` in App
   init). `Scene.cpp` includes no GL headers; bounds come from `MeshData`;
   material-override textures resolve lazily in the GL `RenderSystem`.
   Primitives are agnostic too (`MeshPrimitives`). `MeshData::recenter` /
   `AssetManager::recenterOBJ` handle off-origin authored assets (rock.obj is
   authored 53 units from its origin).
6. **Physics debug-draw extracted — DONE.** `PhysicsSystem` is fully GL-free;
   the collider wireframes live in `Engine/Rendering/PhysicsDebugRenderer`.
7. **`EngineCore` static library — DONE.** Core + Scene + Input + agnostic
   Assets + Jolt physics; links glm/stb/tinygltf/ufbx/tinyobjloader/glfw/
   sol2/Jolt — **no glad, no imgui**. `Engine` is now just the GL layer on
   top of it. (DestructionSystem stays GL-side: it builds OBJModels.)
8. **Per-frame `VulkanRenderSystem` + dynamic TLAS — DONE.** `VulkanAccel`
   keeps one TLAS + instance/scratch buffer per frame in flight;
   `drawFrame()` rebuilds the frame's TLAS from the instance list every
   frame (barrier to fragment-shader ray queries included), so transforms
   are fully dynamic. `VulkanRenderSystem` walks the ECS per frame
   (hierarchy-aware world transforms, Lifecycle/visible), resolves meshes
   through the AssetManager (`createMeshFromData`) or by OBJ path, and
   re-finalizes when new meshes appear.
9. **True `glGenVk` runtime — DONE (first cut).** `VulkanRHI/runtime/main.cpp`:
   GLFW_NO_API window + surface + EngineCore, world built via
   `Scene::spawnFromFile`, dynamic rock pile simulated by the engine's Jolt
   `PhysicsSystem` on a static floor, RT shadows tracking the motion.
   Not yet wired: scripting/gameplay/audio subsystems from `Runtime/`.

10. **Editor UI on Vulkan — DONE (shell).** `VulkanRHI/runtime/VkEditor.{h,cpp}`
   runs the old editor's UI on glGenVk: EditorTheme + EditorToolbar (reused
   headers, GL-free), Hierarchy / Inspector (Transform, Mesh, Rigidbody,
   Collider, Add Component) / Assets browser (double-click spawns via
   `Scene::spawnFromFile`) + Console (engine Logger) / Environment /
   Statistics panels, scene New/Save/Load through the GL-free Scene
   serialization, and ImGuizmo gizmos (Y-X-Z Euler decomposition, unflipped
   projection, W/E/R + snap from the toolbar). Uses its own
   `imgui_glgenvk.ini`. The full `EditorUI.cpp` panels for FX systems
   (sky/clouds/post/black hole) follow with the FX port.

### What's LEFT (in recommended order)
1. **Grow `glGenVk` into the full app** — wire ScriptSystem (Lua), gameplay,
   audio and the `Runtime/Framework` subsystem structure.
2. **Port FX** — sky + fog already done in Vulkan; port clouds, fire, volumetric
   fog, and the `black_hole_raymarch.comp` compute effect (modernized); then
   bring the corresponding EditorUI panels across.
3. **Delete OpenGL** — remove glad + GL renderer/FX; drop the dependency.

## Gotchas / lessons (so they aren't rediscovered)
- **GLM clip space:** build with `GLM_FORCE_DEPTH_ZERO_TO_ONE` and flip
  `proj[1][1] *= -1` for Vulkan.
- **Ray queries** require GLSL `#version 460` (450 fails: "rayQueryEXT
  undeclared"). Compile shaders with `glslc --target-env=vulkan1.3`.
- **Mesh-shader device features:** do NOT blindly enable the whole queried
  `VkPhysicalDeviceMeshShaderFeaturesEXT` — `multiviewMeshShader` /
  `primitiveFragmentShadingRateMeshShader` need deps you won't enable; set them
  FALSE.
- **ImGui 1.92.6** `InitInfo`: MSAA + rendering formats live in nested
  `PipelineInfoMain`; there's an `ApiVersion` field. The Vulkan backend wasn't
  vendored — pulled from `ocornut/imgui` v1.92.6 into `ThirdParty/imgui/backends/`.
- **VMA** isn't bundled in the SDK — FetchContent'd (v3.1.0) in `VulkanRHI/CMakeLists.txt`.
- **`_`-prefixed AppleDouble junk:** the repo has macOS `._*` files scattered
  about (CMake globs already exclude them). If `.git/objects` ever gets `._*`
  files they break `git gc` — delete `find .git -name '._*' -delete`.

## Commit history on `vulkan-engine` (Vulkan work)
foundation → bindless/PSO → mesh+materials → HDR/tonemap → RT shadows → mesh-shader
terrain → ImGui UI → free-fly camera → terrain generator + `run-vulkan.bat` → sky →
terrain materials → fog → multi-object scene → engine-drivable API → **Material
decoupled** → **ECS-driven scene** → **Input decoupled** → **asset parse/upload
split** → **physics debug-draw extracted** → **EngineCore carved (no GL)** →
**per-frame VulkanRenderSystem + dynamic TLAS** → **glGenVk runtime (EngineCore +
Jolt physics + VulkanRHI)**. (`main` is untouched old OpenGL.)

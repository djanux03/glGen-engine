# glGen — Vulkan Port: Status & Handoff

> Written before a machine format so nothing is lost. This captures where the
> OpenGL→Vulkan rewrite stands and how to build/run it.

## TL;DR

- **Goal was:** a *fully* Vulkan engine with the **same functionality** as the
  old OpenGL engine (UI/editor, mouse-look/zoom, terrain generator, etc.)
  **plus** modern Vulkan features (ray tracing, mesh shaders, bindless, …).
- **Status: OpenGL has been fully removed.** `glGenVk` is now the **only**
  runtime and the only build target — there is no more `-DGLGEN_BUILD_VULKAN`
  toggle, no `glGen` executable, no `glad`, no GL `Engine`/`Editor` libraries.
  `main` (untouched old OpenGL) still exists as a historical reference branch,
  but `vulkan-engine` (this branch) no longer builds OpenGL at all.
- **Painterly Vulkan revamp:** the renderer now keeps its physical
  Cook–Torrance GGX direct lighting, atmospheric sun radiance, IBL, SSAO,
  ray-traced visibility, fog, volumetrics, bloom, and HDR pipeline while
  applying the illustrative treatment at the material and display stages.
  Terrain uses five authored lit/shade paint ramps with optional albedo-only
  overlays, pigment mottling, wash edges, and mountain faceting. Vegetation
  keeps physical lighting and adds albedo-tinted transmitted sunlight via
  the versioned scatter-manifest `foliageSssStrength` field.
- **Assets:** current pine, grass, and rock meshes are intentional
  placeholders. No missing birch or future-stylized mesh path is registered;
  replacement assets can be adopted through the scatter manifest without
  renderer changes.
- **What was explicitly skipped, by decision, not oversight:** FX porting
  (clouds, fire, volumetric fog, the black-hole compute raymarch) and the CPU
  `TerrainSystem` (brush/raycast/vegetation) were never ported to Vulkan —
  they were deleted along with the rest of OpenGL rather than rewritten. This
  is a real, permanent visual downgrade versus the old engine (no clouds,
  fire, volumetric fog/light shafts, black hole, or cascaded shadow maps —
  shadows are now hardware ray-traced instead) until/unless someone picks
  FX porting back up as new work.

## Build & run

Prereqs: **Vulkan SDK** (LunarG; dev used 1.4.350.0 at `C:/VulkanSDK/<ver>`),
CMake, Visual Studio 2026 ("Visual Studio 18 2026" generator), an RTX/RT-capable
GPU (dev used an RTX 3070).

**Easiest path:** run `run-glgenvk.bat` from the repo root — configures,
builds, and launches `glGenVk` in one step (auto-detects `VULKAN_SDK` and the
CMake install location).

**Manual path:**
```
cmake -S . -B Build-vs18 -G "Visual Studio 18 2026" -A x64
cmake --build Build-vs18 --config Release --target glGenVk
.\Build-vs18\bin\Release\glGenVk.exe
```
(No `-DGLGEN_BUILD_VULKAN=ON` needed anymore — Vulkan is the only backend, it
always builds. `VULKAN_SDK` must be set in the environment, or CMake's
`find_package(Vulkan)` will fail; `run-glgenvk.bat`/`run-vulkan.bat` set it
for you by scanning `C:\VulkanSDK\*` if it isn't already set.)

**Controls:** **RMB** = mouse-look (free-fly debug camera), **scroll** = zoom
(FOV), **WASD + Space/Ctrl** = fly (Shift = fast) while RMB is held. Editor:
**W/E/R** = gizmo move/scale/rotate, double-click an asset in the Assets
panel to spawn it, **Ctrl+S** saves the scene.

**Headless verification:** env `GLGEN_SMOKE_FRAMES=N` auto-exits after N
frames; `GLGEN_SMOKE_CAPTURE=<path.png>` writes a PNG of the last frame —
useful for confirming a change renders correctly without a display session.
`GLGEN_SMOKE_STYLE_POSE=meadow|forest|mountain` selects a deterministic
camera/sun/exposure pose. `scripts/capture-painterly.ps1` captures all three.

There's also a `glGenVulkanSmoke` target (`run-vulkan.bat`) — the original
Phase-0 smoke-test executable from before `glGenVk` existed; it still builds
and runs but `glGenVk` is the real thing to use.

**Tests:** opt-in, `-DGLGEN_BUILD_TESTS=ON` (see `Tests/CMakeLists.txt`).

## What the Vulkan renderer (`VulkanRHI/`) does

Vulkan 1.3 core throughout (dynamic rendering + synchronization2), verified
runtime-clean with validation layers on:

- **Foundation:** instance+validation, physical-device selection requiring RT +
  acceleration-structure + mesh-shader, VMA allocator, swapchain, 2 frames in
  flight, PSO cache (disk-persisted).
- **Bindless** textures (partially-bound, update-after-bind array).
- **Per-frame UBO** ring (replaces immediate-mode uniforms).
- **Mesh draw path:** OBJ loader (tinyobjloader), per-material submeshes,
  stb_image sRGB textures, device-local vertex/index buffers via staging, depth.
- **HDR** offscreen target + painterly/ACES/Reinhard/linear diagnostic
  tonemap modes, warm-highlight/cool-shadow split tone, vibrance, vignette,
  depth-derived outlines, edge-aware smoothing, and paper finish.
- **Hardware ray-traced shadows** (BLAS/TLAS + ray query `rayQueryEXT`) — the
  only shadow technique; there is no cascaded-shadow-map fallback anymore.
- **GPU-driven terrain** via **mesh shaders** (`VK_EXT_mesh_shader`): task shader
  frustum-culls patches, mesh shader generates a displaced grid. Live generator
  params. Slope/height materials (sand/grass/rock/snow). (Note: this is a
  *rendering-only* terrain — there's no CPU-side `TerrainSystem` anymore for
  brush editing, raycasting, or vegetation placement; see "What's gone".)
- **Procedural sky** + day/night (sun elevation driven by the UI light),
  with broad painted grading and real-sun-oriented cloud bands shared by the
  visible sky and environment cubemap.
- **Aerial/distance fog**, day/night aware.
- **Free-fly camera** (mouse-look/zoom/WASD).
- **Dear ImGui** UI on the Vulkan backend (imgui_impl_vulkan, dynamic rendering).
- **Multi-object scene:** meshes + instances with per-object transforms; TLAS
  over all instances.
- **Headless PNG capture** for verifying visuals.
- **Subsystem-managed startup** (`Engine/Core/SubsystemManager`, the same
  dependency-ordered manager the old GL app used): `Window → VkAudioSubsystem
  → VkPhysicsSubsystem → VkScriptSubsystem → VkEditorSubsystem`.
- **Lua scripting** (`ScriptSystem`, sol2) — entities with a `ScriptComponent`
  get `on_spawn`/`on_update` called; the demo world's `Player` entity has one.
- **Audio** (`VkAudioSubsystem`, miniaudio-based) — ambient loop + footstep
  cadence tied to WASD movement.
- **Gameplay** — ported player controller (mouse-look/move/jump), player
  interaction (raycast grab, destructible damage — see "What's gone" for the
  one piece that's stubbed), and spaceship control, all driven by the ECS.

### `VulkanRHI/` layout
`VulkanContext` (instance/device/VMA), `VulkanSwapchain`, `VulkanBindless`,
`VulkanPipelineCache`, `VulkanMesh` (OBJ load), `VulkanAccel` (BLAS/TLAS),
`VulkanRenderer` (the renderer + engine-drivable API), `vk_mem_alloc_impl.cpp`.
Shaders in `shaders/vulkan/` (mesh, terrain.task/mesh/frag, sky, tonemap).

`VulkanRHI/runtime/` is the actual `glGenVk` app:
- `main.cpp` — window/device setup, demo world, subsystem registration, main loop.
- `VkAppState.h` — the Vulkan-side counterpart of the old GL app's `AppState`.
- `VkEditor.{h,cpp}` — the editor UI (Hierarchy/Inspector/Assets+Console/
  Environment/Statistics panels, ImGuizmo gizmos). The Environment panel is
  tabbed (Sky/Light/Fog/Camera/Post/Terrain/Debug) with a cross-tab settings
  filter box; `Windows > Reset Layout` restores the tiled default layout.
- `EditorCamera.h` — the editor viewport camera: mouse-only (RMB look, MMB
  pan, scroll dolly with momentum), exponentially smoothed, zoom/pan speed
  adaptive to height above terrain. **Editor mode has no keyboard movement
  by design** — that's the editor/play split: entering Play (menu-bar button)
  hides all panels, shows an orange PLAY banner + viewport border, captures
  the mouse and hands WASD/mouse to the player character; Esc stops, restores
  the scene snapshot AND the pre-play editor camera pose. Gameplay input
  systems (player controller, interaction, spaceship), Lua scripts and
  footstep audio (`VkAppState::playState`, kept in sync by the main loop) all
  gate on play mode.
- `EditorState.h`, `EditorTheme.h`, `EditorToolbar.h`, `GameplayState.h`,
  `InputSettings.h` — small GL-free headers relocated here from the now-deleted
  `Editor/`/`Runtime/Framework/` directories; glGenVk is fully self-contained.
- `subsystems/` — `VkWindowSubsystem`, `VkPhysicsSubsystem`, `VkScriptSubsystem`,
  `VkAudioSubsystem`, `VkEditorSubsystem`, `VkCoreAppLayer` (per-frame update
  order).
- `gameplay/` — `VkPlayerControllerSystem`, `VkPlayerInteractionSystem`,
  `VkSpaceshipControlSystem` (ported from the old `Runtime/Gameplay/`, adapted
  to `VkAppState`).

## What's gone (deleted with OpenGL, not ported)

- **All OpenGL code**: `Engine/Rendering/*` (Shader/Texture/Renderer/GLStateCache/
  CloudFX/FireFX/HDRSky/PostProcessor/SunFX/BlackHole*/PhysicsDebugRenderer/
  GLModelBackend/GLDebug — only `Material.h`, a pure data struct, survives),
  `Engine/Assets/{OBJModel,FBXModel,UFBXModel}` (the GL GPU-upload half of
  assets), `Engine/ECS/Systems/RenderSystem.h`, the old `glGen` executable
  (`Runtime/`), the GL `Editor` library (`Editor/EditorUI.cpp`, `MousePicking.h`),
  and the `glad` + GL-`imgui` dependencies.
- **`Engine/Terrain/` in full** (`TerrainSystem`, `TerrainGpuRenderer`,
  `TerrainBrushSettings`, `TerrainMaterialSettings`) — `TerrainSystem` returned
  `OBJModel::VertexData` throughout and was never wired into glGenVk in the
  first place, so it was deleted wholesale rather than rewritten. glGenVk's
  mesh-shader terrain (above) is rendering-only — no brush editing, raycasting
  against terrain, or procedural vegetation placement exists anymore.
- **`Engine/ECS/Systems/DestructionSystem`** — its `.cpp` pulled in
  `OBJModel`/`FBXModel`/`UFBXModel`/`TerrainSystem`, all deleted. Effect:
  destructible entities still take damage and their health still reaches 0 in
  `VkPlayerInteractionSystem`, but no fracture/shard entities spawn — there's
  no Vulkan-side destruction visual yet.
- **`Runtime/Gameplay/ProjectileSystem`, `FireflySystem`** — both drew via raw
  GL calls; not ported, not wired into glGenVk (their simulation logic was
  never called either, since nothing consumes it without a renderer).
- **Clouds, fire billboards, volumetric fog (post-process), black-hole
  compute raymarch** — never touched; these existed only in the OpenGL FX
  layer, which is now deleted. Cascaded shadow maps: same fate, superseded by
  the RT shadows above rather than ported.

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
  `PipelineInfoMain`; there's an `ApiVersion` field.
- **VMA** isn't bundled in the SDK — FetchContent'd (v3.1.0) in `VulkanRHI/CMakeLists.txt`.
- **`stb_vorbis.c` macro pollution:** it `#define`s single-letter macros
  `L`/`C`/`R` for channel constants and never undefines them. If anything
  compiled after it (e.g. `miniaudio.h`, which does `#include <windows.h>`
  internally) triggers a *first* parse of `<winnt.h>`, its `RUNTIME_FUNCTION`
  bitfield members (also named `R`/`L`/`C`/`H`) get corrupted into
  macro-expansions, producing bizarre "syntax error: 'constant'" cascades deep
  in Windows SDK headers. Fix: `#include <windows.h>` yourself, first, before
  anything else, in any TU that also does `#include "stb_vorbis.c"`.
- **MSVC runtime-library mismatch on a truly clean build:** Jolt Physics'
  own `ThirdParty/JoltPhysics/Build/CMakeLists.txt` has an `OVERRIDE_CXX_FLAGS`
  option (default ON) that wholesale-replaces `CMAKE_CXX_FLAGS_DEBUG`/
  `_RELEASE`, which drops the runtime-library marker CMake needs to emit a
  correct `<RuntimeLibrary>` for those two configs — causing `LNK2038`
  "RuntimeLibrary mismatch" (`/MT` vs `/MD`) against everything else in the
  project. Root `CMakeLists.txt` now forces `CMAKE_MSVC_RUNTIME_LIBRARY` and
  `OVERRIDE_CXX_FLAGS=OFF` via `CACHE ... FORCE` to keep this from
  regressing. This is pre-existing and unrelated to the Vulkan port — it just
  never surfaced because nobody had done a fully-clean rebuild before.
- **`_`-prefixed AppleDouble junk:** the repo has macOS `._*` files scattered
  about (CMake globs already exclude them). If `.git/objects` ever gets `._*`
  files they break `git gc` — delete `find .git -name '._*' -delete`.

## Commit history on `vulkan-engine` (Vulkan work)
foundation → bindless/PSO → mesh+materials → HDR/tonemap → RT shadows → mesh-shader
terrain → ImGui UI → free-fly camera → terrain generator + `run-vulkan.bat` → sky →
terrain materials → fog → multi-object scene → engine-drivable API → Material
decoupled → ECS-driven scene → Input decoupled → asset parse/upload
split → physics debug-draw extracted → EngineCore carved (no GL) →
per-frame VulkanRenderSystem + dynamic TLAS → glGenVk runtime (EngineCore +
Jolt physics + VulkanRHI) → **editor UI shell on Vulkan** → **subsystem-managed
startup + scripting/audio/gameplay (glGenVk feature parity, minus FX)** →
**OpenGL fully removed** (FX porting and CPU TerrainSystem explicitly skipped,
not carried over). `main` remains the untouched old OpenGL engine for reference.
# Painterly renderer (2026-07)

The Vulkan runtime now ships a painterly-only lighting/material path:
three-band colored lighting, watercolor terrain ramps and faceted mountains,
painted sky/clouds, depth-derived outlines, chroma-preserving tonemapping,
edge smoothing, paper grain, autumn vegetation grading, and per-scatter-layer
rim strength. Cook-Torrance terrain/scene shading and the procedural grass
optics stack have been retired. `assets/settings/style_presets.json` contains
the Painterly Summer and Golden Hour authored presets; the editor exposes the
same live style block.

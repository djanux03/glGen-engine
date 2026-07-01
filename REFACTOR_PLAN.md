# glGen Refactor Plan

A staged plan to take this engine from a solid-but-rough 6/10 codebase to a maintainable 7.5–8. The work targets **engineering quality**, not user-visible changes — players will not notice these refactors. Future feature work and performance work both get cheaper afterwards.

## Ordering principle

You can't safely refactor without tests. You can't write meaningful tests against a god struct. So the order is forced:

**Phase 1 (tests) → Phase 2 (AppState diet) → Phase 3 (god-file splits) → Phase 4 (API hygiene).**

Doing them in any other order means either flying blind or rewriting tests after each refactor.

---

## Phase 1 — Test foundation

**Why critical:** ~30k LOC, custom ECS, custom asset pipeline, custom physics integration, **zero pre-existing tests**. Any subsequent refactor risks silent breakage. ECS bugs especially are silent — they don't crash, they just put a transform on the wrong entity three weeks later.

**Approach:** doctest, pulled via FetchContent. Tests live in [Tests/](Tests/) with a CMakeLists that works two ways:

- Standalone (fast TDD, no engine compile): `cmake -S Tests -B Build-tests`
- Subdir of root project: `cmake -B Build -DGLGEN_BUILD_TESTS=ON`

Tests deliberately do **not** link the Engine library. They cover header-only or otherwise self-contained code. When tests need engine code, add a separate target rather than coupling the whole test binary to the engine build.

### Status

| Item | Status |
|---|---|
| Test target builds and runs | ✅ Done |
| CTest integration | ✅ Done |
| `SparseSet<T>` ([Engine/ECS/SparseSet.h](Engine/ECS/SparseSet.h)) | ✅ 13 cases |
| `Registry` ([Engine/ECS/Registry.h](Engine/ECS/Registry.h)) | ✅ 13 cases |
| `SubsystemManager` ([Engine/Core/SubsystemManager.cpp](Engine/Core/SubsystemManager.cpp)) | ✅ 10 cases |
| `ProjectConfig` ([Engine/Core/ProjectConfig.cpp](Engine/Core/ProjectConfig.cpp)) | ✅ 7 cases |
| `EventBus` ([Engine/Core/EventBus.h](Engine/Core/EventBus.h)) | ✅ 6 cases |
| `PerlinNoise` ([Engine/Core/PerlinNoise.h](Engine/Core/PerlinNoise.h)) — known-value regression | ✅ 7 cases |
| GitHub Actions CI job running `ctest` on push | ✅ Done |

### Phase 1 acceptance

- `ctest` runs in <2s
- ECS, SubsystemManager, ProjectConfig, EventBus, PerlinNoise all covered
- CI is green on `main`

### Things Phase 1 explicitly does NOT do

- No GL/render snapshot tests yet — fixture cost too high. Wait until Phase 4.
- No physics tests — Jolt is too heavy a dep to drag into the test binary right now.
- No coverage target — running tests is the deliverable, not measuring them.

---

## Phase 2 — Put `AppState` on a diet

**Why critical:** The subsystem architecture in [Engine/Core/IEngineSubsystem.h](Engine/Core/IEngineSubsystem.h) is genuinely good — phases, dependencies, topological initialization. But it's undermined by every subsystem holding `AppState&`. That's why [CoreAppLayer.h:15-19](Runtime/Framework/CoreAppLayer.h:15) declares dependencies on 9 subsystems — it has to, because everything reads/writes shared state through `AppState`.

`AppState` ([Runtime/Framework/AppState.h](Runtime/Framework/AppState.h)) currently holds **~160 fields**. This blocks every other improvement:
- You can't unit-test a subsystem without instantiating the entire engine
- You can't split god files cleanly because the slices keep reaching back into shared state
- Every new subsystem becomes another field on the god struct

**This phase is the highest-value change in the entire plan**, and the riskiest. Do not start until Phase 1 tests are green and CI is set up.

### Approach

Audit `AppState` field-by-field. Bucket each field into one of:

| Bucket | Destination |
|---|---|
| Truly cross-cutting (window handle, frame timing, profiler) | New slim `EngineContext` struct |
| Subsystem-owned config/state (`RenderSettings`, `AudioSettings`, `TerrainBrushSettings`) | Move into the owning subsystem |
| Editor-owned UI state (`SelectionState`, `HistoryState`, `PendingActions`) | Move into `EditorSubsystem` |
| Gameplay state (`playerId`, `axeEntity`, `grabbedEntityId`, `woodCount`, hotbar/viewmodel offsets, `debugGameplay*`) | Move into `PlayerControllerSystem` / `PlayerInteractionSystem`, or onto components on the player entity |
| Diagnostic/debug overlay (`debugGameplay*`, `debugGrab*`) | New `DebugOverlay` struct accessed only by the editor |

After bucketing:
1. Replace `AppState&` constructor parameter on each subsystem with **explicit DI** of just what it actually needs (`EngineContext&` + its own state + pointers to the specific subsystems it calls).
2. `CoreAppLayer`'s dependency list shrinks naturally as fields move out.
3. Goal: `AppState` (or its successor) drops from ~160 fields to ~15.

### Progress

- [x] Extract `AudioSettings` out of `AppState` into `AudioSubsystem` and its own header.
- [x] Extract `TerrainBrushSettings`.
- [x] Extract Editor UI state (`SelectionState`, `HistoryState`, `PendingActions`).
- [x] Extract Gameplay state.
- [ ] Extract `RenderSettings` (last).

### How to roll it out without breaking everything

- **One sub-struct per PR.** Move `AudioSettings` first (smallest, most isolated). Then `TerrainBrushSettings`. Then the editor UI state. Save `RenderSettings` for last — most fields, most cross-cutting.
- For each PR: write/extend a test for the affected subsystem first, then refactor, then verify ctest is still green.
- Use `git grep` to find every reader/writer of each field before moving it. The compiler won't catch behavioral drift.

### Phase 2 acceptance

- Each subsystem header declares the real, narrow set of dependencies it has
- `CoreAppLayer`'s dependency list has shrunk
- `AudioSubsystem` (or any other subsystem) can be constructed and tested without `AppState`
- The successor to `AppState` has fewer than 20 fields

---

## Phase 3 — Split the god files

**Why critical:** Three files account for ~11,000 lines and block any kind of meaningful contribution or code review:

| File | Lines | Plan |
|---|---|---|
| [Editor/EditorUI.cpp](Editor/EditorUI.cpp) | 4342 | Split per-panel: `EditorUI_Outliner.cpp`, `EditorUI_Inspector.cpp`, `EditorUI_Viewport.cpp`, `EditorUI_AssetBrowser.cpp`, `EditorUI_SkyPanel.cpp`, `EditorUI_TerrainPanel.cpp`, `EditorUI_Console.cpp`. Each panel becomes a free function or small class taking an `EditorContext&`. |
| [Engine/Terrain/TerrainSystem.cpp](Engine/Terrain/TerrainSystem.cpp) | 3589 | Split by concern: `TerrainChunkLoader.cpp` (streaming), `TerrainHeightmap.cpp` (generation), `TerrainBrush.cpp` (editor edits), `TerrainScatter.cpp` (vegetation placement). |
| [Runtime/Framework/CoreAppLayer.cpp](Runtime/Framework/CoreAppLayer.cpp) | 3223 | Extract: console-command handling, history snapshotting, drop-file ingestion, terrain raycasting helpers — each into its own translation unit. |

### Approach

1. **`EditorUI.cpp` first.** Biggest file, easiest cuts (panels are already logically separable in the source).
2. **No behavior changes.** Each PR at this stage is pure file-moves with `git mv`-style splits and targeted include updates. Run the editor manually before/after each split to catch surprise regressions.
3. After each split, **add at least one snapshot/integration test** for that area. Even something as crude as "with these inputs the terrain heightmap hash is X" catches regressions.

### Phase 3 acceptance

- No source file > 1000 lines (rare exceptions allowed)
- Build time stays flat or improves
- Editor still works end-to-end

---

## Phase 4 — Renderer API hygiene

**Why critical:** The renderer's public API is silting up — early-warning sign of a system that will calcify. Easier to fix now than after another year of features.

### Concrete tasks

1. **Replace [`Renderer::setFrameUniforms`](Engine/Rendering/Renderer.h:31)** (40+ parameters) with a `FrameUniforms` struct passed by const-ref. Group: lighting, fog, NPR (toon/rim/ramp), ambient.
2. **Delete the dead `init` / `initWithShadows` dual signature.** Pick one, drop the "kept for compatibility" comment, update callers.
3. **Remove the unused `sidePath` / `topPath` / `bottomPath` parameters** — the comment in [Renderer.h:16](Engine/Rendering/Renderer.h:16) admits they're unused.
4. **Resolve the `FBXModel` / `UFBXModel` / `GLTF` naming confusion** in [Components.h:28-49](Engine/ECS/Components.h:28). Either:
    - Rename `FBXModel` → `GLTFModel`, `UFBXModel` → `FBXModel`, `MeshComponent::AssetType::GLTF` stays, OR
    - Drop `FBXModel` entirely if `UFBXModel` covers FBX adequately, and use a separate dedicated GLTF loader.
5. **Now is when render-output snapshot tests become feasible** — add at least one golden-image diff for a known scene/camera/lighting combo.

### Phase 4 acceptance

- No public function in `Renderer` takes >8 parameters
- No two functions have near-identical signatures
- Class names match what they actually load
- At least one render snapshot test exists

---

## Phase 5 — Polish (defer, do not let distract)

These came up in the review but are **explicitly not critical**. Don't let them block the four phases above.

- README typos and screenshot dates ("Visual Studio 18 2026", aspirational)
- Assets committed to repo — move to git-LFS (one-day chore)
- Empty `saveConfig` / `loadConfig` stubs in [App.cpp:90-96](Runtime/Framework/App.cpp:90) and [CoreAppLayer.cpp:47](Runtime/Framework/CoreAppLayer.cpp:47) — delete when the new state model lands in Phase 2
- Lua scripting story (commit to it or remove — but that's a product question)
- Migrating off OpenGL 4.1 / adding an RHI layer — multi-month rewrite, not a fix

---

## Pre-existing repo issues to fix before/during Phase 1

- **`ThirdParty/JoltPhysics/` is empty.** Looks like an uninitialized git submodule. The full engine cannot build until this is resolved (`git submodule update --init`, or vendor Jolt properly).
- **No CI.** Phase 1 includes adding a GitHub Actions workflow.
- **No `.editorconfig`** — minor, but trivial to add.

---

## Estimated timeline (single developer, focused)

| Phase | Effort |
|---|---|
| 1 — Tests | 1 week (≈30% done in initial session) |
| 2 — AppState diet | 2–3 weeks (incremental, one sub-struct per PR) |
| 3 — God-file splits | 1–2 weeks |
| 4 — Renderer / API hygiene | 1 week |

**~6 weeks total.** Moves the engine from a 6/10 to a comfortable 7.5–8.

---

## Non-negotiables

1. **Never start Phase 2 before Phase 1 has the ECS + Subsystem tests green and a CI job running them.** This is the single most important rule in this plan.
2. **One concern per PR.** Don't bundle "split EditorUI.cpp" with "fix the renderer API" with "tests". Each phase, each step, ships independently and is independently revertable.
3. **Run the editor end-to-end after every Phase 2 sub-struct move and every Phase 3 file split.** Tests catch logic regressions; manual smoke catches "the gizmo doesn't draw anymore" regressions that escape the test net.
4. **No new features during this refactor.** Adding a feature mid-refactor multiplies risk and conflict surface. Park feature work; ship the refactor; then resume.

---

## What this plan deliberately does not address

- Performance (frame time, draw calls, shadow cost)
- Graphics fidelity (PBR, IBL, GI, TAA wiring)
- Game-side polish (level content, game-feel)

If those become priorities, a separate plan is required. Combining them with this refactor will tank both.

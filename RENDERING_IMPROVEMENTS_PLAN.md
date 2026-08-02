# glGenVk Rendering Improvements — Implementation Handoff

Self-contained work plan for the rendering-quality phases on the
`vulkan-engine` branch. Phases 1–2 (bug fixes + hardening) and Phase 3
(3.1–3.5) are **done and visually verified**. Phase 4 (4.1 SSAO half-res, 4.2
auto-exposure, 4.3 bloom second blur) is **fully implemented and compiles
clean** (VulkanShaders + glGenVk + full-solution build all pass with no new
warnings), but **4.1 and 4.3 have not been visually verified** — no capture
was taken for either (see their checklist entries). If something in SSAO
contact shadows or bloom glow looks off, those two are the first place to
look. Everything below is a record of what changed and how it was verified —
read "Context" and "Invariants" before touching anything if picking this
back up.

## Context: what was already fixed (2026-07-17)

The terrain used to render with giant rainbow bands. Root cause:
`terrainChunk.frag` re-declared a *prefix* of the FrameData UBO and its
`terrainMat3` field landed on `camPosWS`'s bytes (offset 304 vs the real 336),
so "macro variation strength" was literally the camera's Z coordinate —
albedo went negative/huge and the ACES tonemapper turned that into rainbows.

Fixed by:
- `shaders/vulkan/frameData.glsl` — the FrameData UBO (set=1, binding=0) is
  declared **once** there and included whole (via `GL_GOOGLE_include_directive`)
  by all four scene shaders: `mesh.vert`, `meshInstanced.vert`, `mesh.frag`,
  `terrainChunk.frag`.
- `static_assert`s on `FrameDataGpu` offsets in `VulkanRHI/VulkanRenderer.h`
  (currently 304/320/336/352, sizeof 368).
- Negative-value guards in `tonemap.frag`; firefly clamp `[0, 64]` in
  `bloomExtract.frag`.
- Debug view modes: `Params::debugViewMode` → `miscParams.x` (0=off 1=albedo
  2=normals 3=fog 4=SSAO 5=shadowVis 6=NaN detector), implemented at the end
  of `mesh.frag` and `terrainChunk.frag`; tonemap becomes linear passthrough
  while active. Editor UI: Environment panel → "Debug Views".
- World-scale defaults in `VulkanRenderer.h` Params: fog density 0.006 /
  start 30 / new `fogHeightRef` (-2), terrain bands in meters
  (grass -1..2, snow 10..16), far plane 420 m
  (`VulkanRenderer.cpp`, `glm::perspective` call), shadow-ray tMax 420 in
  both frag shaders.

## Invariants — do not break these

1. **Never re-declare a prefix of FrameData in a shader.** Add fields by
   editing `shaders/vulkan/frameData.glsl` AND `FrameDataGpu` in
   `VulkanRenderer.h` (append-only, vec4-aligned), then update the
   static_asserts (offsets + sizeof) in the same header. If a static_assert
   fires, the two sides drifted — fix the layout, don't delete the assert.
2. New shader files must be added to `VK_SHADERS` in
   `VulkanRHI/CMakeLists.txt`; new `.glsl` includes must be added to the
   `DEPENDS` list of the glslc custom command there.
3. Keep `mesh.frag` and `terrainChunk.frag`'s debug-view switch working — if
   you rename `albedo`/`N`/`vis`/`ssao`/`fogAmt` locals, update the switch.
4. Don't change `Params` field defaults casually: the editor UI
   (`VulkanRHI/runtime/VkEditor.cpp`, `drawEnvironment`) sliders must cover
   any new default ranges.

## Build & verify workflow (Windows, this machine)

- cmake is NOT on PATH: use `"/c/Program Files/CMake/bin/cmake.exe"` (Git
  Bash) or `& "C:\Program Files\CMake\bin\cmake.exe"` (PowerShell).
- Build dir is `Build-vs18` (already configured with `-DGLGEN_BUILD_VULKAN=ON`).
  - Shaders only (fast, catches GLSL errors):
    `cmake --build Build-vs18 --target VulkanShaders --config Release`
  - Runtime: `cmake --build Build-vs18 --target glGenVk --config Release`
    → `Build-vs18/bin/Release/glGenVk.exe`
- Headless visual verification (validation layers are on; errors go to
  stderr):
  `GLGEN_SMOKE_FRAMES=180 GLGEN_SMOKE_CAPTURE=<path>.png ./Build-vs18/bin/Release/glGenVk.exe`
  then LOOK at the PNG. Capture fires on the last frame; 180 frames lets
  terrain chunks stream in. Run from the repo root.
- Two debug env-var hooks already exist in `VulkanRHI/runtime/main.cpp`
  (guarded by `getenv`, zero effect if unset — safe to leave in place, add
  more the same way):
  - `GLGEN_SMOKE_DUSK=1` — sets `lightPitchDeg = 8` for a repeatable
    low-sun/dusk capture (used by 3.1/3.2/3.3 verification).
  - `GLGEN_SMOKE_FOGTEST=1` — pulls the camera back to
    `(0, 8, 40)`/`pitch -6` and raises fog density/lowers fog start, so the
    terrain/sky horizon actually reaches fog opacity in a capture (used by
    3.3 to check the fog/sky seam). Combine with `GLGEN_SMOKE_DUSK=1`.
  - `GLGEN_SMOKE_SURVEY=1` — high angled overview, `(0, 60, 60)` pitch -45,
    lowered fog density, to see across the whole streamed chunk radius
    instead of just the flat near-origin drop zone (used by 3.4). The
    default smoke scene has no dramatic cliffs, so this doesn't stress-test
    slope-dependent shading (e.g. triplanar detail) much -- fine for
    "did I break anything" but not "does this look great on a mountain".
  - `GLGEN_SMOKE_NIGHT=1` — sun below the horizon (`lightPitchDeg = -30`),
    distinct from `GLGEN_SMOKE_DUSK`'s grazing angle; for auto-exposure's
    dark end of the curve (used by 4.2).
  - `GLGEN_SMOKE_AUTOEXP=1` — sets `Params::autoExposure = true`. Combine
    with `GLGEN_SMOKE_NIGHT=1` to see it ramp up, or alone at default noon
    lighting to confirm it converges back to `autoExposureMin` (used by 4.2).
- Verify **every task below** with a capture before moving to the next. For
  lighting tasks also capture a second shot with "Light pitch" low (dusk) by
  temporarily setting `Params::lightPitchDeg = 8.0f` — day AND dusk must both
  look sane.

---

## Phase 3 — Lighting & shading quality

### Task 3.1 — Move the sky model into a shared include

The sky gradient/day/dusk/night function exists twice, hand-synced:
`sky.frag` (`skyColor()`, includes sun disc) and `mesh.frag` (`skyAmbient()`,
no sun disc). Any tweak to one silently diverges the other.

- Create `shaders/vulkan/skyModel.glsl` containing one function:
  `vec3 skyBase(vec3 dir, vec3 sun, float nightBrightness, float duskStrength)`
  — the shared gradient/day/dusk/night/ground-haze logic (everything in
  `mesh.frag`'s `skyAmbient()` today, parameterized instead of reading
  `uFrame`/`pc` directly, since sky.frag has no FrameData binding).
- `sky.frag`: `skyBase(...)` + its existing sun disc/glow term on top.
- `mesh.frag`: `skyAmbient(dir, sun)` becomes a thin wrapper calling
  `skyBase(dir, sun, uFrame.skyAmbientParams.x, uFrame.skyAmbientParams.y)`.
- Add `skyModel.glsl` to the CMake `DEPENDS` list (invariant #2).
- Verify: capture must be pixel-identical-ish to before (no intended visual
  change).

### Task 3.2 — Unify terrain lighting with mesh.frag (biggest visual win)

`terrainChunk.frag` currently uses a flat scalar ambient and a nonstandard
formula: `color = albedo * (ambient + (1.0 - ambient) * direct)` where
`ambient = lightParams.x * ssao`. Props (mesh.frag) get sky-colored
hemisphere irradiance — so terrain looks flat gray next to objects and
ignores time-of-day color.

- Include `skyModel.glsl` in `terrainChunk.frag` (needs the wrapper reading
  `uFrame.skyAmbientParams`, same as mesh.frag).
- Replace the lighting combine with the same structure mesh.frag uses for its
  diffuse-only case:
  ```glsl
  vec3 skyUp = skyAmbient(vec3(0,1,0), L);
  vec3 skyDown = skyAmbient(vec3(0,-1,0), L);
  vec3 irradiance = mix(skyDown, skyUp, 0.5 + 0.5 * N.y);
  vec3 ambient = albedo * irradiance * ssao * uFrame.lightParams.x;
  vec3 direct = albedo * ndl * vis * uFrame.lightDir.w;
  vec3 color = ambient + direct;
  ```
  (Terrain stays Lambert — no specular needed.)
- IMPORTANT tuning note: today's `lightParams.x` default (0.2) multiplied a
  *scalar* 1.0; irradiance is ~0.4–0.7, so terrain will darken. After the
  change, tune `Params::ambientIntensity` default (try 0.5–0.7) so terrain
  brightness roughly matches the previous capture at noon, and check props
  (mesh.frag also multiplies `lightParams.x` into ambient) still look right —
  they share the slider, which is the point.
- Verify: noon capture (terrain grass should be comparable brightness to
  before, slightly blue-tinted in shadowed areas) + dusk capture (terrain
  should warm up / darken with the sky, no more gray flatness).

### Task 3.3 — Sky-consistent fog color (aerial perspective)

Both frag shaders currently fog toward a constant `fogDayColor`/`fogNightColor`
mix, which never matches the procedural sky at the horizon (visible seam).

- In `mesh.frag` and `terrainChunk.frag`, replace the fog color computation:
  ```glsl
  vec3 viewDir = normalize(vWorldPos - uFrame.camPosWS.xyz);
  vec3 fogColor = skyAmbient(normalize(vec3(viewDir.x, max(viewDir.y, 0.02), viewDir.z)), L);
  ```
  (Clamping `y` slightly above the horizon avoids sampling the dark
  below-horizon haze for downhill look angles.)
- Optional sun forward-scatter for depth (cheap, big payoff at dusk):
  `fogColor += sunTint * pow(max(dot(viewDir, L), 0.0), 8.0) * 0.25 * day;`
  where `sunTint = mix(vec3(1.0,0.55,0.25), vec3(1.0,0.96,0.88), day)`
  (same constants sky.frag uses).
- Keep `fogDayColor`/`fogNightColor` Params/UBO fields in place (editor still
  binds them) but they become unused by these two shaders — note that in a
  comment in frameData.glsl. Do NOT remove UBO fields (invariant #1).
- Verify: horizon seam between fogged terrain and sky should nearly vanish
  in both noon and dusk captures.

### Task 3.4 — Terrain material polish

- **Band-edge dithering**: the snow/grass lines are perfectly horizontal.
  Perturb the height before banding:
  `float hDither = h + (valueNoise(vWorldPos.xz * 0.12) - 0.5) * 3.0;`
  and use `hDither` in the two `smoothstep` band mixes (keep `h` for
  everything else). The `3.0` (meters of wobble) can be hardcoded first;
  promote to `miscParams.z` if tuning is needed.
- **Triplanar detail on steep slopes**: the detail texture is sampled on XZ
  only and smears on cliffs. Blend three planar samples weighted by
  `abs(N)` (normalized so weights sum to 1), using
  `uFrame.terrainMat2.z` scale on each plane. Only worth it for the detail
  texture; keep macro/worley noise as-is.
- Verify: capture near a slope/mountain (teleport camera by temporarily
  setting `Params::camPos` in `VulkanRHI/runtime/main.cpp` around line 351,
  which already overrides it) — no stretched streaks on cliff faces, wobbly
  snow line.

### Task 3.5 — Shadow-ray distance fade

Every pixel currently traces up to 4 shadow rays even where fog is ~90%
opaque and shadows are invisible.

- In both frag shaders' `main()`, skip tracing when the fog will swallow the
  result: compute the fog factor **before** lighting (it only depends on
  `vViewZ`/`vWorldPos`), and if `fogAmt > 0.85 * uFrame.fogParams.z`, set
  `vis = 1.0` and skip `shadowVis()`. Restructure the code so fog variables
  are computed once, above the lighting block.
- Verify: identical-looking capture; if you want numbers, the Stats panel
  FPS at 1080p should tick up slightly.

## Phase 4 — Post-processing & SSAO

### Task 4.1 — SSAO half-res + bilateral upsample (perf) [optional, skip if time-boxed]

Full-res 16-tap SSAO with derivative normals is the priciest screen pass.
- Render `mSSAOImages` at half resolution (see `createSceneTargets()` in
  `VulkanRenderer.cpp` — mirror what bloom does with `halfExtent`), keep the
  blur pass, and make the blur depth-aware (sample depth, reject
  contributions where |Δdepth| large) so the upsample doesn't bleed across
  edges. `ssao_blur` currently lives in `blur.frag`.
- Verify: capture diff — AO under rocks/trees still present, no halos at
  object silhouettes.

### Task 4.2 — Auto-exposure (simple)

Fixed exposure 1.1 can't serve both noon and dusk once Task 3.2/3.3 land.
- CPU-side approach (no new shaders): each frame, in `drawFrame()`, you know
  sun elevation (`-lightDir.y`); derive a target exposure curve, e.g.
  `target = mix(2.2, 1.1, smoothstep(-0.1, 0.3, -lightDir.y))`, then ease
  `mParams.exposure` toward it (`exposure += (target - exposure) * 0.05`)
  **only when** an `autoExposure` Params bool (add it + editor checkbox) is
  true. This is deliberately not histogram-based — keep it simple.
- Verify: dusk capture no longer pitch-dark, noon unchanged.

### Task 4.3 — Bloom quality (only if it looks bad after 3.x)

Current: half-res threshold extract + one separable blur → small hard halo.
If dusk sun bloom looks steppy: add a second blur iteration at quarter res
(one more image pair + reuse `mBloomBlurPipeline`), summed in
`tonemap.frag` — do NOT rewrite into a full mip chain; not worth it here.

---

## Ordering & scope guidance

Do 3.1 → 3.2 → 3.3 in one session (they build on each other), capture-verify
between each. 3.4/3.5 are independent. Phase 4 tasks are independent of each
other; 4.2 should come after 3.2/3.3 since those change scene brightness.

Commit per task (or per 3.1–3.3 block) on the `vulkan-engine` branch; don't
push without being asked. Update this file's checkboxes as you go:

- [x] 3.1 shared sky include (skyModel.glsl: skyGradient/applyGroundHaze/skyBase; sky.frag and mesh.frag now call it; verified noon+dusk captures match prior behavior. Added `GLGEN_SMOKE_DUSK=1` env hook in main.cpp -- sets lightPitchDeg=8 for repeatable dusk captures, reuse for 3.2/3.3.)
- [x] 3.2 terrain lighting unification (terrainChunk.frag now uses skyAmbient() hemisphere irradiance, same structure as mesh.frag's diffuse term; bumped Params::ambientIntensity default 0.2->0.4 to compensate for the flat-scalar->irradiance-magnitude change; verified noon+dusk captures, terrain/prop brightness comparable to before Task 3.2, rocks show clearer shading definition now.)
- [x] 3.3 sky-consistent fog (mesh.frag + terrainChunk.frag fog color now sampled from skyAmbient(viewDir, L) + sun forward-scatter tint, replacing the flat fogDayColor/fogNightColor mix; verified via GLGEN_SMOKE_FOGTEST=1 env hook in main.cpp -- pulls camera back + raises fog density so the horizon seam is actually visible in a capture. Noon+dusk both show a seamless terrain-fog-to-sky blend, no more hard color line at the horizon.)
- [x] 3.4 terrain material polish (band edges now dithered via valueNoise(worldXZ*0.12) wobble on a height copy used only for the sand/grass/snow smoothsteps; detail texture is now triplanar, blended by |N| per axis instead of a single XZ-planar sample. Verified via new `GLGEN_SMOKE_SURVEY=1` main.cpp hook -- birds-eye camera (0,60,60) pitch -45 over the streamed chunks; renders cleanly, no seams/artifacts, ~470 FPS. Default smoke scene has no dramatic cliffs, so the triplanar fix is confirmed compiling/rendering correctly but not visually stress-tested on a real slope -- worth a look once real mountain terrain is available.)
- [x] 3.5 shadow-ray distance fade (mesh.frag + terrainChunk.frag: fogAmt is now computed before shadowVis() and rays are skipped once `fogAmt >= 0.85 * fogParams.z`; fog color/mix reuse the same fogAmt further down instead of recomputing. Verified via GLGEN_SMOKE_FOGTEST=1 -- correct rendering, no artifacts, no validation errors. Did not get a clean perf A/B: 3.4's added triplanar/dither cost landed in the same session, so raw FPS numbers aren't isolated to this change alone.)
- [x] 4.1 SSAO half-res + bilateral upsample (mSSAOImages, the raw-AO target, now allocated at halfExtent -- same half-res the bloom chain uses -- while mSSAOBlurImages stays full-res since the scene pass samples it directly via gl_FragCoord. blur.frag now takes a 2nd descriptor set [full-res depth, reusing mSSAODepthSets] and a BlurPush{invProj} push constant: each of its 5x5 taps is weighted by exp(-|Δview-space-Z|*2.0) against the full-res depth, so the half-res AO doesn't bleed across silhouette edges when upsampled -- effectively a combined blur+bilateral-upsample in the existing pass, no new pipeline. createBlurPipeline()'s layout is now 2 set layouts + 1 push constant range; drawFrame() renders the SSAO pass at a new half-res viewport/scissor and binds {mBlurInputSets, mSSAODepthSets} + pushes invProj for the blur pass. Verified: VulkanShaders target compiles (blur.frag), full glGenVk target builds clean, and a full-solution build shows no new errors/warnings anywhere else in the tree. NOT visually verified this session per instruction -- no headless capture was taken; the depth-weight sharpness constant (2.0 in blur.frag) is a first guess and may need tuning once someone looks at it, especially at silhouette edges (rock-against-terrain, tree-against-sky).)
- [x] 4.2 auto-exposure (Params::autoExposure/autoExposureMin/Max/Speed added; VulkanRenderer.cpp eases `mParams.exposure` toward a sun-elevation target each frame using the same day/night curve the shaders use, before building FrameDataGpu. Editor UI: checkbox + sliders in Post-Processing, disables the manual Exposure slider while active. Verified via GLGEN_SMOKE_AUTOEXP=1 [+GLGEN_SMOKE_NIGHT=1, new lightPitchDeg=-30 hook distinct from DUSK's grazing angle] -- noon converges to autoExposureMin=1.1 (matches prior fixed-exposure noon captures), night converges to autoExposureMax=3.2 and turns a near-black fixed-exposure night frame into a readable one.)
- [x] 4.3 bloom second blur (implemented proactively at the user's request, not because 3.x captures showed a problem -- so unverified visually, same caveat as 4.1. Added a 2nd quarter-res H/V blur pass: mBloomBlurQ2HImages/mBloomBlurQ2VImages, sized at halfExtent/2, created/destroyed alongside the existing bloom targets in createSceneTargets()/destroySceneTargets(). Reuses mBloomBlurPipeline (bloomBlur.frag already reads textureSize(uSource,0), so it self-adapts to any resolution -- no shader change needed there). createBloomResources()/updateBloomSets() grew from 3 to 5 descriptor sets per frame (added Q2H sampling mBloomBlurVViews -- the downsample happens for free via the linear sampler, same trick bloomExtract.frag uses -- and Q2V sampling Q2H). drawFrame()'s drawBloomFullscreen lambda now takes extent/viewport/scissor as parameters instead of closing over one fixed half-res set, so it can drive both the half-res and new quarter-res passes. tonemap.frag gained a 3rd sampler binding (uBloomWide) and sums it in at 0.6x the main bloomIntensity so the wide pass reads as a soft outer falloff, not a doubled core; createTonemapResources()/updateTonemapSets() grew from 2 to 3 bindings/writes accordingly. Verified: VulkanShaders + glGenVk + full-solution builds all clean, no new warnings. NOT visually verified -- no capture taken. The 0.6x wide-pass weight is a first guess; if the glow looks too heavy or too faint, that's the one number to retune (VulkanRenderer.cpp's drawFrame() tonemap push doesn't carry it -- it's hardcoded in tonemap.frag's `* pc.bloomIntensity * 0.6`, so promote it to a Params field + push constant if it needs a slider instead of a recompile.)

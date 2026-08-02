# Graphics Revamp — Painterly Surfaces × Realistic Lighting — Design Plan (v2)

**Status: IMPLEMENTATION SOURCE OF TRUTH.** A partial v1 ramp-lighting
implementation exists in the working tree and must be replaced by this v2
direction; the final renderer keeps physical GGX lighting and removes all
v1 band-lighting fields/includes.

Target, in the art brief's own words:

> A stylized, painterly video game landscape, vast temperate wilderness, dense
> pine and birch forest with golden autumn foliage, low-poly jagged granite
> mountains, rolling grassy hills. Soft watercolor texture mapping, high
> contrast, clean minimalist shapes, sharp outlines. Serene atmospheric haze,
> afternoon sun breaking through canopy casting long crisp shadows. The Long
> Dark art style but summer, 3D game engine asset style, cinematic composition.

**Direction (v2, per art direction decision):** *The Long Dark* look done the
way TLD actually does it — **hand-painted, minimalist surfaces lit by
believable, physically-real light**. The engine's realistic direct-lighting stack
(ray-traced shadows, physical atmosphere, sky IBL, volumetric shafts, HDR) is
**kept and pushed further**, not replaced. Ambient light may inherit the
subtly art-directed sky cubemap, and display color is graded after ACES.
All other stylization happens in what the light *hits* (albedo, normals,
roughness, geometry) and in how the final frame is *finished* (grade,
outlines, paper). No cel/ramp shading, no fake tinted
shadows — cool blue shadows come from real skylight fill, which the pipeline
already computes.

*(v1 of this plan proposed banded ramp lighting; superseded. The old
`MEADOW_TERRAIN_REVAMP_PLAN.md` — which pushed toward photo-sourced PBR
texturing — has been deleted; its surviving infrastructure decisions are
restated here so nothing depends on the deleted file. Code comments still
citing it are historical and can be cleaned up opportunistically.)*

---

## 1. Frame anatomy today (what the analysis found)

The only runtime is `glGenVk` (Vulkan 1.3, `VulkanRHI/`). One frame currently
runs, in order:

| # | Pass | Source | Notes |
|---|------|--------|-------|
| 1 | Depth-only Z-prepass | scene/terrain vert stages, no frag | feeds SSAO; main pass is test-only |
| 2 | SSAO half pipeline | `ssao.frag` → `blur.frag` | R8, darkens ambient only |
| 3 | Env-cubemap (6 faces + mips) | `sky.frag` / `skyModel.glsl` | 128² RGBA16F per frame; IBL + fog color source |
| 4 | Scene pass → HDR RGBA16F | `sky.frag` (fullscreen), `terrainChunk.frag`, `mesh.frag`, `meshInstanced.frag` | GGX PBR + ray-query shadows (TLAS), SSAO, IBL, biome lighting, fog |
| 5 | Ray-traced volumetrics (half-res) | `volumetric.frag` → `volumetricComposite.frag` | god rays; HG anisotropy, turbulence |
| 6 | Bloom | `bloomExtract.frag` → separable blurs (half + quarter res) | additive in tonemap |
| 7 | Tonemap → swapchain | `tonemap.frag` | exposure (auto), ACES, saturation/contrast, vignette, chromatic aberration, camera biome grade; ImGui overlay |

Key facts that shape this plan:

- **The lighting stack is already the right foundation.** Cook-Torrance GGX
  in all three lit shaders, CPU-mirrored atmospheric transmittance coloring
  the direct light (free golden hour), hardware ray-traced shadows with cone
  softness (the *only* shadow tech — and exactly what "long crisp shadows"
  wants), per-frame sky cubemap IBL (so ambient and fog always match the
  sky), thin-surface SSS on props/foliage, ray-marched volumetric shafts.
  This is the "insanely realistic lighting" half of the target, ~done.
- **The surfaces are the problem.** Terrain shades a 5-layer *photo* splat
  (meadow/forest/dirt/rock/scree × albedo/normal/roughness, planar+triplanar,
  anti-tiling, height-lerp — ~15 bindless taps/pixel) **plus**
  `grassMaterial.glsl`, a 995-line procedural realism stack (species patches,
  tussocks, blade strands, dew, puddles, cuticle sheen, additive optics).
  Photo texture density is the opposite of "clean minimalist shapes / soft
  watercolor texture mapping."
- **Color pipeline**: ACES + auto-exposure + a small biome camera grade. A
  solid filmic base; what's missing is *art direction* on top (split toning,
  palette bias), and chromatic aberration is a photo artifact that fights a
  painted frame.
- **No outline/edge pass exists** (old GL `outline.frag` was a selection
  highlight), **no clouds exist at all** (GL CloudFX was deleted, never
  ported), and **no anti-aliasing pass** exists in the Vulkan chain (old
  FXAA/TAA were GL-only).
- Terrain geometry: CPU-meshed streamed chunks (`Engine/Terrain/`), smooth
  analytic per-vertex normals, and per-vertex fields (curvature, rock mask,
  continuous biome weights `(wMeadow,wForest,wMountain)`) delivered to the
  fragment stage — a ready-made set of painter's masks.
- Vegetation: GPU-instanced batches with per-instance color jitter + baked
  biome weights, vertex wind, per-batch draw distance and TLAS opt-out. The
  *system* is exactly right; the *assets* (one pine OBJ + primitives) and the
  photo-real material response are not.
- Editor (`VkEditor.cpp`, tabbed Environment panel) live-drives everything
  through `VulkanRenderer::Params` → `FrameDataGpu` (append-only UBO with
  static_asserted offsets). Headless verification exists:
  `GLGEN_SMOKE_FRAMES=N` + `GLGEN_SMOKE_CAPTURE=<png>`.

### Gap list (current pipeline vs. target)

1. **Photo splat + procedural blade realism** → maximal surface noise. Brief
   wants big flat watercolor fields with pigment character.
2. **Per-pixel normal/roughness maps everywhere** → photographic micro-relief
   and speculars. Painted forms want broad, smooth light gradients.
3. **No outlines**, no drawn-shape delineation.
4. **Smooth terrain normals everywhere** → no low-poly jagged granite.
5. **Cloudless physically-blue sky** → no painted-sky composition; haze is
   physically colored but not *art-directed*.
6. **Summer-green pine only** → no birch, no golden foliage.
7. **CA + neutral grade** → photographic finish instead of a painted one.
8. **No AA** → any thin drawn line will crawl.

---

## 2. Art direction pillars (decisions, not options)

- **P1 — Big value shapes.** A frame reads as ~5–9 large value/color masses
  (sky / mountains / forest wall / hills / shadow pools). Surface detail may
  *texture* a mass, never shatter it. High contrast lives between masses.
- **P2 — Real light on painted surfaces.** Light transport stays physical:
  real sun transmittance, real sky fill (which makes shadows naturally cool
  and blue), real ray-traced occlusion, real aerial perspective. Stylization
  never fakes lighting; it simplifies *materials* so the real light draws
  broad, clean gradients instead of micro-noise. Where realism can be pushed
  cheaply, push it (§4.4).
- **P3 — Watercolor surface.** Albedo = flat ramp colors + low-frequency
  pigment mottling + paper grain; near-flat normals; unified, high, mostly
  map-free roughness for organics. No photo textures on organic surfaces.
- **P4 — Drawn edges.** Thin, dark, distance-faded outlines on silhouettes
  and hard creases. Crisp RT shadow edges are embraced as part of the look.
- **P5 — Serene depth.** Haze bands, painted clouds, golden volumetric
  shafts. The atmosphere system does the cinematic work; defaults put the sun
  at ~25–30° elevation (long shadows, warm key) with light haze.

---

## 3. Keep / delete / replace

### Kept and central (the realistic-lighting half)
- GGX Cook-Torrance direct lighting, Smith visibility, Schlick Fresnel, the
  env-BRDF ambient specular — all three lit shaders.
- Ray-traced shadows (TLAS ray queries, cone softness) — unchanged tech;
  defaults tuned crisper (§4.4).
- Physical sky (`skyModel.glsl`) + CPU transmittance mirror + sun/moon
  handoff + starfield. Untouched as the lighting source of truth.
- Per-frame sky env cubemap → IBL + fog color (this is what keeps every
  stylization consistent across time-of-day for free).
- SSAO, thin-surface SSS (foliage backlight — *this* is the physically
  honest version of "rim light"), volumetrics, bloom, HDR + auto-exposure.
- All infrastructure: chunk streaming, mesher + per-vertex biome/curvature
  fields, scatter/instancing, bindless, editor, smoke-capture harness,
  FrameData append-only discipline.

### Kept but re-aimed
- **Terrain material plumbing** (`terrainMaterialSlots`, bindless resolve,
  tiling, height-lerp/biome blending logic): kept, but slots now feed
  *painterly* maps/ramps instead of photo PBR sets (§5).
- **`biomeLighting.glsl`** (canopy dapple, biome fog tint/density, mountain
  aerial term): kept as-is — it's plausible-light-level art direction, which
  is exactly this plan's philosophy. Gets retuned, not rewritten.
- **`tonemap.frag`**: ACES stays the base transform (it's a good filmic
  spine for high-contrast realism); gains grading layers on top (§7.1).

### Deleted / replaced
- Chromatic aberration (delete outright).
- Photo texture sets as the terrain's default look source (plumbing stays;
  defaults stop referencing them).
- ~85% of `grassMaterial.glsl`'s realism micro-model (species/dew/puddles/
  sheen/glint additive optics and the `GM_OPTICS_ENERGY` block in
  `terrainChunk.frag`). Grass keeps clump/stroke structure and LOD fades;
  loses the jewelry (§5.3).
- Per-pixel normal-map relief as a default on organics (maps stay supported
  for props; terrain layers default to flat or heavily-flattened normals).

---

## 4. Materials & light: how "painterly" meets "realistic" (new shared rules)

No new lighting model. Instead, a small shared include
`shaders/vulkan/paintMaterial.glsl` provides the *material-side* stylization
helpers used by `terrainChunk.frag`, `mesh.frag`, `meshInstanced.frag`:

### 4.1 Painterly albedo, real BRDF
- Albedo is authored (ramps + mottle + strokes, §5), then fed to the
  **unchanged** GGX/IBL/shadow pipeline. The light does the modeling; the
  paint does the color.
- Organic roughness: single scalar per layer/material (high, ~0.8–0.95), no
  roughness maps on terrain/foliage → speculars become broad soft sheens,
  not photographic glitter. Props may keep maps (`materialFlags` path stays).

### 4.2 Form simplification (the TLD trick)
- **Flatten normals**: terrain layer normal maps default off; the
  grass bump normal survives only at very reduced strength (it currently
  fights per-triangle geometry normals anyway). Smooth vertex normals + real
  sun = the long soft gradients that read "painted."
- **Facet where the brief says jagged** (§5.4): mountains get geometric
  faceting — *real* geometry-derived normals, so the realistic light model
  renders real crystalline planes. Low-poly look without faking light.

### 4.3 Foliage backlight, physically honest
- The existing thin-surface SSS term (`mesh.frag`) is promoted into the
  shared include, given a per-batch strength (`VegBatch` field) and an
  albedo-colored transmission tint — sun through a golden birch crown glows
  gold because transmission is albedo-tinted, not because of a rim hack.
  `meshInstanced.frag` adopts it; terrain drops its dead grass-optics version.

### 4.4 Lighting-realism upgrades (cheap pushes, same tech)
Funded by the surface-cost savings (§11):
- **Shadows**: default `shadowSoftness` slightly up with `shadowSamples=4`
  kept — real penumbra growth with distance while contacts stay crisp
  (afternoon-sun look). Re-measure; grass is already TLAS-excluded.
- **Env cubemap 128² → 256²** + one extra mip: cleaner sky gradients in IBL
  and fog, sharper water/prop reflections. Trivial cost (still 6 tiny faces).
- **SSAO retune** for larger-radius, softer form occlusion (form shading,
  not crevice dirt), which suits big smooth shapes.
- **Volumetrics**: unchanged tech; retuned warmer/wider (§6.3).
- *(Stretch, post-S7)*: ray-queried AO (short rays vs. TLAS, quarter-res) to
  replace SSAO's screen-space artifacts; evaluated only after the style ships.

### 4.5 FrameData additions (append-only, per the struct's rule)
```
vec4 stylePaint0;  // x=mottleStrength y=mottleScale z=paperGrainStrength w=edgeDarkenStrength
vec4 stylePaint1;  // x=facetStrength y=autumnAmount z=foliageSssBoost w=masterStyleDial
vec4 styleOutline; // rgb=line color w=line width(px)
vec4 styleOutline2;// x=depthThresh y=normalThresh z=distanceFade w=wobble
vec4 styleGrade0;  // rgb=shadow split-tone color w=split balance
vec4 styleGrade1;  // rgb=highlight split-tone color w=vibrance
vec4 styleSky0;    // x=gradeStrength y=washBands z=washSoftness w=sunSoftness
vec4 styleCloud0;  // x=coverage y=softness z=windX w=windY
vec4 styleCloudLit, styleCloudMid, styleCloudBase;
vec4 terrainPaintLit[5];   // rgb=lit ramp color w=roughness
vec4 terrainPaintShade[5]; // rgb=shade ramp color w=overlayStrength
```
`masterStyleDial` (0 = today's look, 1 = full painterly surfaces) gates the
material-side changes during bring-up for A/B smoke captures; lighting is
never gated because it doesn't change. Removed in final cleanup.

---

## 5. Terrain & ground: watercolor material, GGX response

`terrainChunk.frag` keeps all inputs and its blending logic (biome weights,
curvature, rock/scree masks, height-lerp) — those become the painter's masks
— and swaps what they select.

### 5.1 Ramp palette instead of photo splat
- Each of the 5 layers becomes a **2-color ramp** (litColor, shadeColor —
  selection driven by mottle + slope/moisture, *not* by light; lighting stays
  GGX's job) plus per-layer roughness scalar. 5×2 editor colors replace 15
  texture maps. `terrainMaterialSlots` stays; a slot may optionally point at
  a *hand-painted* overlay texture for mid-distance interest, default empty.
- Autumn accent: forest-biome ground warms toward ochre with the
  `autumnAmount` dial; meadow stays summer green ("TLD but summer" with
  golden accents).

### 5.2 Pigment mottling + paper grain + edge darkening (P3)
- **Mottle**: 2–3 octaves of the existing `valueNoise` (~8 m and ~1.5 m)
  drive ramp position — watercolor pigment pooling.
  `terrainMacroVariationStrength` is re-labeled/re-scaled for this.
- **Paper grain**: new tileable luminance texture
  (`assets/terraingeneratorassets/paper_grain.png`, ~512²), sampled
  world-planar on terrain (~0.5 m tiling, strength ~0.05) and again
  screen-space in post (§7.3).
- **Edge darkening**: darken ramp output where material masks transition
  (`fwidth` of rockWeight/biome weights) — pigment settling at wash borders.

### 5.3 Ground detail rewrite (`grassMaterial.glsl`)
- Keep: L2 tussock Worley cells + L3 oriented blade strokes **as albedo
  strokes** (hue/value jitter per clump), the excellent distance/fwidth LOD
  dissolve, and a *weak* bump only where it helps close-up form.
- Delete: species mosaic, dew/glint, puddles/water term, sheen, translucency
  micro-model, soil grit — and the whole additive `groundOptics` block in
  `terrainChunk.frag`. Foliage translucency lives in the honest SSS term
  (§4.3) on actual grass-card instances instead.
- Target: ~200 lines, no additive light terms, ~12 fewer taps/pixel.

### 5.4 Low-poly jagged granite
- **Shader-side faceting**: `Nf = normalize(cross(dFdx(vWorldPos),
  dFdy(vWorldPos)))`, blended by `wMountain * rockWeight * facetStrength` —
  continuous like every other biome transition. Real facet planes under real
  sun = hard crystalline value breaks, honestly lit.
- Mesher ridge sharpening is explicitly **out of scope for v2**. Shader
  faceting plus the existing ridged terrain noise is the shipped solution.
- Granite palette: cool blue-grey shade ramp, warm lit ramp (painted-granite
  complementary pair) — the *sun* provides the warm, the ramp just doesn't
  fight it.

---

## 6. Sky, clouds, atmosphere

### 6.1 Sky: physical base, gentle grade
- `skyModel.glsl` untouched. `sky.frag` gains a **subtle** stylization grade
  applied to both the visible sky and the env-cubemap faces (so the
  deliberately art-directed ambient IBL and fog inherit it automatically —
  the single most important consistency lever):
  small saturation/hue bias (teal zenith, cream horizon in afternoon) and an
  authored horizon-haze color blend. **No posterization by default** —
  banding fights realistic lighting; a wash-banding knob ships default 0 for
  experimentation.
- Sun disc: slightly larger/softer via existing `sunDiscIntensity` + a new
  softness param.

### 6.2 Clouds (new — nothing exists today)
- 2D domain-warped fbm layer on a sky dome in `sky.frag`, shared by env
  faces: coverage/softness params, wind drift, **lit plausibly** — sun-side
  warm edge from the real sun transmittance color, base shaded by sky
  in-scatter color. Painted in shape (soft, simple, large), realistic in
  light. Cheap; no volumetric clouds and no billboards.
- Rendering into the cubemap means clouds correctly tint IBL, fog, and water.

### 6.3 Haze & volumetrics (P5)
- Fog keeps sampling the env cubemap (palette-consistent by construction).
  Retune: slightly higher `fogStart`, gentler density, biome fog tints
  louder (`forestFogTint` toward mossy gold-green).
- Volumetrics stay as-is technically; retune **fewer, warmer, wider**:
  lower `volumetricDensityScale`, anisotropy ~0.8, warm tint already
  supported (`volumetricTintColor`) — soft wedges of afternoon light through
  canopy, not laser beams.

---

## 7. Post stack

### 7.1 Grade (`tonemap.frag`) — first phase to ship
- **ACES stays** the base transform (high contrast, realistic highlight
  handling). On top, in display space: **split toning** (shadows toward cool
  blue-violet, highlights toward warm sun; two colors + balance — this
  delivers P2's warm/cool statement without touching lighting), **vibrance**
  (saturation that protects already-saturated pixels), existing
  saturation/contrast kept.
- Delete chromatic aberration. Vignette default up to ~0.15 (cinematic
  frame). Auto-exposure kept; range narrowed slightly for a steadier value
  key in daylight.
- The existing camera biome grade (forest/meadow/mountain whole-frame shift)
  is kept — it already implements "stepping under the canopy."

### 7.2 Outline pass (new) — P4
- A dedicated fullscreen pass **after scene+volumetrics** writes an `R8`
  edge mask. It samples Z-prepass depth, reconstructs view normals from
  depth (no G-buffer), and performs Roberts-cross edge detection on depth
  (silhouettes) + normals (creases).
- Style: line color deep warm sepia-navy (never pure black), 1–1.5 px,
  distance-faded (far ridges keep silhouette lines only), faded into fog by
  the same fog math, tiny threshold jitter for hand-drawn wobble.
- Bloom continues to sample HDR scene color only, so neither the soft
  pigment edge nor the final line can enter bloom.
- `tonemap.frag` samples the edge mask, applies pigment darkening and the
  final line after bloom contribution, and writes a per-frame `RGBA8` LDR
  intermediate rather than the swapchain.
- **FXAA ships in the same phase** as a dedicated `fxaaResolve.frag`: it
  samples the LDR intermediate, performs FXAA, applies screen-space paper
  grain, and writes the swapchain. ImGui is drawn afterward.

### 7.3 Watercolor screen finish
- Screen-space paper grain (same §5.2 asset, two scales, strength ~0.04)
  is explicitly bound in the FXAA-resolve descriptor and applied after
  tonemapping. Terrain receives its bindless paper texture index through
  FrameData. Both sites use a neutral flat-noise fallback if loading fails.
- Soft edge pigment darkening: reuse the outline pass's edge mask at low
  strength before the drawn line applies.
- Sub-pixel hand-tremor screen warp is explicitly out of scope for v2.

---

## 8. Vegetation & scatter: placeholders now, asset-ready later

- **Assets are supplied later by the user.** Keep the current pine, grass,
  and rock meshes as placeholders; do not create or activate a layer whose
  mesh path does not exist. Future low-poly pine, birch, shrub, boulder, and
  painted grass-card assets drop in through manifest-relative paths.
- **`terrain_scatter.json`**: retune existing placeholder layers for a dense
  forest wall, open meadows, and restrained mountain scatter. Extend the
  schema with tint, `foliageSssStrength`, wind, alpha-cutout, scale, and
  biome weights. Add `birch_stand` only after a valid mesh exists.
- **`meshInstanced.frag`**: painterly material rules (§4.1–4.2) + the
  promoted albedo-tinted transmission with
  `ScatterLayer::foliageSssStrength -> VegBatch -> ScenePush` (§4.3).
  It applies to instanced vegetation only, not ordinary opaque props.
  Wind and `groundOcclusion` remain.
- **`autumnAmount`** also biases per-instance crown hue by the instance's
  baked `wForest` weight — forest goes gold, lone meadow trees stay green.

---

## 9. Editor integration

- New **"Style" tab** in the Environment panel (`VkEditor.cpp`): terrain
  layer ramp pairs, mottle/paper/edge-darken strengths, facet strength,
  autumn amount, foliage SSS boost, outline (color/width/thresholds/fade/
  wobble), split-tone colors + balance, vibrance, cloud coverage/softness/
  wind, sun-disc softness, master style dial.
- Terrain tab: splat texture path fields collapse to "overlay texture
  (optional)" per layer + the ramp colors.
- **Presets** (serialized style block): `Painterly Summer` (default),
  `Golden Hour`, `Legacy` (dial=0, for A/B until S7 removes it).

---

## 10. Phasing (each phase independently verifiable via smoke captures)

Capture protocol: fixed seed, 3 canned poses (meadow vista / inside forest
toward the sun / mountain ridge), `GLGEN_SMOKE_FRAMES=120` +
`GLGEN_SMOKE_CAPTURE`, diffed against the prior phase's set.

| Phase | Content | Files (primary) | Exit criterion |
|---|---|---|---|
| **S0** | Scaffolding: FrameData `style*` fields + Params + static_asserts, master dial, Style tab skeleton, capture-pose script | `VulkanRenderer.{h,cpp}`, `frameData.glsl`, `VkEditor.cpp` | dial exists; zero visual change at dial=0 |
| **S1** | Grade: split toning, vibrance, CA removed, vignette/auto-exposure defaults; ACES untouched | `tonemap.frag`, `VulkanRenderer.cpp` | warm-light/cool-shadow split reads in captures; lighting numerically unchanged |
| **S2** | Material simplification: flattened normals, unified organic roughness, promoted albedo-tinted SSS in shared include | new `paintMaterial.glsl`, `mesh.frag`, `meshInstanced.frag`, `terrainChunk.frag`, `VulkanRenderer.cpp` | broad smooth light gradients on all geometry; backlit foliage glows; dial=0 remains capture-equivalent |
| **S3** | Terrain watercolor: ramp palette replaces photo splat, mottle, edge darkening, grass-stroke rewrite, groundOptics deleted, mountain faceting | `terrainChunk.frag`, `grassMaterial.glsl`, `frameData.glsl` | terrain reads as painted masses under real light; faceted granite; measurable frame-time drop |
| **S4** | R8 edge mask + LDR tonemap target + FXAA/paper resolve | `outlinePaint.frag`, `tonemap.frag`, `fxaaResolve.frag`, `VulkanRenderer.cpp` | clean 1 px distance-faded lines; no bloom contamination or crawl; resize validation-clean |
| **S5** | Sky/atmosphere: subtle sky grade, painted-shape/real-lit clouds (visible + cubemap), soft sun disc, haze/volumetric retune | `sky.frag`, `VulkanRenderer.cpp` | clouds visible AND reflected in fog/IBL; warm soft canopy shafts |
| **S6** | Placeholder vegetation composition: autumn dials, existing-layer scatter retune, per-batch SSS, asset-ready manifest hooks; no missing birch path | `terrain_scatter.json`, `meshInstanced.frag`, `VulkanRenderer.h` (VegBatch) | placeholder forest wall; three biome masses distinct; future assets require no renderer changes |
| **S7** | Cleanup & lock: master dial removed, dead knobs/paths retired, shadow/SSAO retune, env cubemap 256², defaults = Painterly Summer, perf pass, docs | all touched files | validation-clean; frame time ≤ pre-revamp; docs current |

Order rationale: S1 first because every later capture should be judged under
the final grade; S2 before S3 because terrain ramps are authored against the
simplified material response; outlines/AA (S4) before sky (S5) so cloud work
is judged inside the finished frame finish.

---

## 11. Performance expectations

Net **negative cost** before S4, funding the additions:

- Removed: ~15 splat taps + triplanar math + anti-tiling on every terrain
  pixel (terrain dominates screen coverage), most of the 995-line grass
  stack, the additive optics block.
- Added: R8 edge mask (~5 depth taps), FXAA (~9 LDR taps), paper grain (2 taps),
  cloud noise in sky/env faces (6×256² worst case), env-map res bump.
- Unchanged big-ticket: RT shadow rays (still dominant — grass already
  TLAS-excluded; re-measure penumbra settings in S2), volumetrics, SSAO.

---

## 12. Risks / open decisions

1. **Flat albedo under honest light can read "unfinished"** if roughness and
   SSAO aren't retuned with it — S2 bundles those retunes for exactly this
   reason; judge only on S3+ captures where mottle/strokes exist.
2. **Crisp RT tree shadows on minimalist ground** may read CG-sharp; the
   cone-softness dial gives real penumbra without any tech change. Judge in
   S3 captures.
3. **Outlines on alpha-cut foliage cards** (once stylized cards land) can be
   noisy — silhouette-only lines at distance, crease detection near-field
   only; evaluate in S6.
4. **Asset dependency** (S6) is external; all engine phases verifiable
   without it, by design.
5. **`grassMaterial.glsl` demolition** is the highest-regression-risk edit
   (deeply threaded into terrainChunk.frag). New ground detail lands behind
   the master dial in S3; deletion only happens in S7.
6. **Push-constant budget**: scene push block has 4 floats of headroom — all
   new style data goes through FrameData (append-only), per its rule.
7. **Split toning vs. physical color**: applied in display space after ACES,
   so it can't corrupt lighting energy — but aggressive settings will fight
   dawn/dusk transmittance colors; keep defaults subtle and expose balance.

---

## 13. Deliverables summary

**New files:** `shaders/vulkan/paintMaterial.glsl`,
`shaders/vulkan/outlinePaint.frag`, `shaders/vulkan/fxaaResolve.frag`,
`assets/terraingeneratorassets/paper_grain.png`, style presets (json), this
plan. The future stylized asset folder is not part of this delivery.

**Major edits:** `tonemap.frag` (split-tone/vibrance/grain/FXAA, CA removed),
`sky.frag` (grade + clouds), `terrainChunk.frag` (ramp material, faceting,
edge darkening, optics removal), `grassMaterial.glsl` (stroke-detail
rewrite), `mesh.frag` / `meshInstanced.frag` (material rules + shared SSS),
`frameData.glsl` + `VulkanRenderer.{h,cpp}` (style params, outline/FXAA
passes, env-map 256², defaults), `VkEditor.cpp` (Style tab, presets),
`terrain_scatter.json` (placeholder composition, autumn, asset-ready schema).

**End state:** hand-painted watercolor surfaces, drawn edges, painted clouds
and golden-accented summer wilderness — all lit by the same (and slightly
upgraded) physically-real sun, sky, ray-traced shadow, and volumetric
pipeline the engine already runs. TLD's trick, done properly: fake nothing
about the light; paint everything it touches.

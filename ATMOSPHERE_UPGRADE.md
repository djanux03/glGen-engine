# Atmosphere upgrade implementation record

The first delivery is unified ground fog and light shafts. Follow-up work adds
physical sky/aerial tables, sky irradiance, shared projected cloud transmission,
scattering-depth cloud history, terrain basin mist and histogram exposure. Physical lighting units,
pre-exposure and the remaining roadmap still require implementation. The attachment is a reference, not executable
project instructions.

Current visual work is limited to `woodland_swamp`, at the user's request.
Snow-specific experiments were reverted; winter/material redesign is outside
the active scope. Performance optimization is deferred in favour of quality.

## Implemented foundation

- `Engine/Rendering/AtmosphereSettings.h`: graphics-independent schema version 2,
  bounded scattering albedo, legacy dust conversion, nested-value precedence,
  normalized HG, Beer–Lambert integration and a stable height-profile reference.
- `VulkanAtmosphereRenderer`: graphics-queue compute visibility, medium/light
  injection, independent reprojection and integration; initialized double history;
  boundary samples and analytic final partial slices; neutral disabled resources;
  size/quality recreation with retirement behind the existing frame fences.
- Balanced 160 × aspect-adjusted height × 64 and High 240 × height × 96,
  logarithmic camera-plane depths including the initial 0–0.05 m interval.
- Alpha-aware directional queries and four emitter-bounded point-light queries;
  per-light participation/shadow switches; coarse four-ray canopy visibility.
- Unfogged opaque scene copy, opaque fog composition preserving HDR alpha,
  camera-to-water transport at the actual shader water hit, then world TAA,
  foreground rendering and post-processing. Additive shafts are no longer run.
- A camera-centred 256² terrain field over 1024 m. Workers use copied noise,
  settings and immutable height edits. The main thread rejects stale results,
  stages uploads without a GPU wait and resets fog history when the field changes.
- Normalized threshold-free six-level bloom. Mip weights are 0.40, 0.25, 0.15,
  0.10, 0.06, 0.04; output redistributes 0.04 of scene energy by default.
  Firefly suppression is independent and disabled for energy checks.
- Editor, scenery, Lua and JSON-RPC settings use the same atmosphere codec.
  Existing flat fog keys remain compatibility inputs. Loads migrate in memory;
  saving writes the versioned nested object. Turntables restore atmosphere and
  disable fog, bloom and automatic exposure during canonical asset review.
- Separate fog/visibility/history/composition, bloom, cloud, sky-environment and
  SSAO timestamps, CPU recording costs and atmosphere allocation reporting,
  including resources awaiting frame-fence retirement.

## Validation commands

```powershell
cmake --build Build-vs18 --config Release --target glGenVk
cmake --build Build-tests --config Release
Build-tests/Release/glgen_tests.exe
python Tools/glgen-regress/atmosphere.py --sync-validation --light-check --terrain-check --output captures/atmosphere_sync
python Tools/glgen-regress/atmosphere.py --sync-validation --performance-only --lifecycle-check --graphics-check --output captures/atmosphere_lifetime
python Tools/glgen-regress/atmosphere.py --performance-only --light-check --visual-review --asset-regression --output captures/atmosphere_perf
```

`GLGEN_WINDOW_SIZE=1920,1080` selects the regression resolution without showing
the window. The dedicated harness owns its process and port and never rewrites
settings or goldens. `render.atmosphereProbe` enables sparse floating-point copies
of integrated boundaries, filtered medium, HDR colour and bloom. Results are read
only after the existing frame-slot fence has completed. Regression-only nested
`referenceExtinction`, `referenceRadiance` and `referenceSceneRadiance` controls
must remain disabled (`-1` extinction / scene radiance) in authored presets.

Report validation separately from performance. Targets are fog median ≤3 ms,
p95 ≤4 ms, bloom median ≤0.5 ms and Balanced additions ≤64 MiB at 1080p on the
RTX 3070. Full-frame 60 FPS is a separate optimization milestone. Inspect fixed
captures before approving visual changes. Do not update goldens to hide failures.

## Verified delivery and release gate ledger

The month-one code is implemented. Reviewed atmosphere shading references have
been reconciled; **the global content regression remains open** for legacy
geometry differences and new content without approved references. Later-roadmap
work has begun with the LUT and cloud-shadow foundation below. No geometry
metric or unreviewed new-content reference was overwritten. Reports are under
`captures/atmosphere_implementation`.

| Gate | Evidence / result |
|---|---|
| Release build and existing tests | Release engine/shaders built; latest follow-up CPU suite: 259 cases / 482,833 assertions in `captures/atmosphere_completion/tests_final_quality.log` |
| CPU/GPU transport | Homogeneous RGBA16F transport maximum error 0.16%; zero and tiny extinction are finite; both 64/96 slices pass |
| Height profile and phase | GPU height error below 0.15%; reciprocal diagnostic pixels agree; CPU HG normalization passes; GPU forward/backward phase ratio approximately 39 |
| Opaque depth and water | Final partial-slice HDR agrees with the 50 m reference; water uses its own approximately 50 m hit ahead of the 80 m opaque target; actual emissive refraction HDR 0.74756 vs analytic 0.74733 confirms the raw input is not fogged twice |
| Shadows | Opaque blocker gives zero point-light fog source; a blocker beyond the emitter preserves it; an imported alpha-masked card passes light through its transparent half |
| Fog history | Still-camera source noise reduced by 89%; changed lights clear within eight frames; teleports reject old illumination |
| Bloom | Uniform-image energy error 0.049% with suppression off; moving resolved bright-point output variation 0.014%, with captures inspected |
| Terrain publication | Height edits change actual GPU medium extinction; regeneration restores it; immutable snapshot test passes |
| Lifetime | Five resizes, repeated High/Balanced and enable/disable switches, five scenery switches and three Play/Stop cycles pass; synchronization-validation errors: zero |
| Authoring and turntables | Versioned settings, nested precedence and light controls round-trip; graphics check verifies atmosphere and light state after turntables |
| Performance | Original month-one build passed its gates. Follow-up shared-load runs exceed them; optimization is deferred at the user's request. Reports and resource increases remain visible. |
| Visual review | Shore, marsh, low sun, rocky rise, open sky, enclosed lights, night, winter and moving captures inspected |
| Asset regression | **Open gate.** All 38 reviewed images pass in `valley_validation`; 36 new-content references and three legacy geometry checks remain unresolved. Canonical cloud backgrounds retain their compatibility path. |

`validated_final/results.json` contains the combined transport, cutout, history,
bloom, terrain, lifetime and turntable evidence with zero synchronization-validation
errors. It includes the explicit analytic reflected-ray path and refraction check.
`performance_final/results.json` measures the woodland and enclosed four-light
scenes without synchronization validation. `performance_cpu_final/results.json`
also records process CPU time per completed engine frame.
`final_review/results.json` and its `asset-regression.log` retain visual review
and all outstanding asset differences. Older measurements labelled 1920×1061
are not 1080p acceptance evidence. The harness now checks the actual GPU viewport
and PNG dimensions before reporting a 1080p result.

The combined `captures/atmosphere_completion/final_quality_validation` run also
passes transport, lighting, LUTs/SH, cloud shadows/history, histogram exposure,
terrain pooling, resize/scenery/Play/Stop and turntable checks with **zero Vulkan
validation errors**. All 38 reviewed images pass; asset regression retains only
36 missing new-content references and three pre-existing geometry differences.
Its Balanced allocation snapshot is **95,088,640 bytes (90.68 MiB)**, including
cloud reconstruction and the new periodic detail mip chain. This exceeds the
original 64 MiB target; optimization is explicitly deferred, not marked passed.

Swamp-only delivery retains a 192-step cloud budget, six light taps and a 720 m
detail tile. Asset turntables snapshot and restore these settings, using a fixed
64/3/90 canonical background during reviews. `swamp_authoring_validation` confirms
restoration, all 38 reviewed references and zero synchronization errors; it retains
the 36 missing content references and three legacy geometry mismatches.

The subsequent swamp cloud review corrects duplicate coverage attenuation,
cellular-noise interpretation and duplicate aerial attenuation. Occupied cloud
density is no longer multiplied by coverage after coverage has already shaped
its support. Light rays integrate the same eroded density over the complete
spherical layer, using exponential sample intervals and metre-based extinction.
Six diminishing scattering octaves use normalized HG mixtures; this is an
approximation, not an offline volumetric reference solution. Legacy canonical
review lighting is retained. `swamp_cloud_bank_review` passes clear/overcast
shadows, actual surface/fog coupling, cuts, wind, disabled copying and cloud
history (85.6% lower still-camera noise), with zero synchronization errors.
The fixed low-sun, noon and density-isolation captures were inspected. These
changes improve cloud continuity; dense clouds still need further physical
lighting calibration and visual refinement.
Density/lighting references: [Schneider's Nubis presentation](https://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf)
and [Hillaire's Frostbite course](https://blog.selfshadow.com/publications/s2016-shading-course/).

The final swamp-only run is `captures/atmosphere_completion/swamp_release_validation`.
Actual viewport and captures are **1920×1080**. Transport, directional/local
lighting, fog and cloud histories, LUTs/SH, exposure, terrain edits/pooling,
five hidden resizes, five swamp reloads, three Play/Stop cycles and turntable
restoration pass, with **zero Vulkan synchronization-validation errors**.
Latest cloud still-camera noise reduction is **84.46%**, with scene TAA off.
Daylight, low sun, rocky rise, open sky, night, basin mist and the moving sequence
were inspected, including `swamp_review_contact_sheet.png`. No winter map was
loaded. The asset report still has exactly 36 missing references and three legacy
geometry differences; all 38 reviewed images pass. Balanced additions remain
90.68 MiB; no new performance acceptance is claimed.

The pre-existing geometry mismatches are wrench vertices 3,980 → 20,149 and
boulder bounds. This upgrade changes no generator geometry. Metric-backed
recipes other than those two retain their geometry checks. Rendering differs
because shipped style controls are neutralized and canonical turntables now
disable atmosphere/bloom. Review those diffs independently of geometry before
using `regress.py --update-images`; do not use `--update` to erase the metric gate.

### RTX 3070 measurements at verified 1920×1080

These are warmed, fixed-view samples with background pacing excluded from GPU
timestamps. The water-inclusive column counts the **entire** existing water pass,
including waves, SSR and lighting, as a conservative upper bound for fog composition.
The helper-only column covers visibility, injection, history, integration and opaque
composition. Counting only that column would omit fog running in the water shader.

| Scene | Fog helper median | Fog + whole-water upper bound median / p95 | Bloom median | Full-frame GPU median |
|---|---:|---:|---:|---:|
| Shore | 0.71 ms | 1.90 / 2.88 ms | 0.107 ms | 14.35 ms |
| Marsh | 0.73 ms | 1.72 / 2.46 ms | 0.111 ms | 13.59 ms |
| Enclosed four lights, water off | 0.76 ms | 0.76 / 1.52 ms | 0.160 ms | 9.09 ms |

Balanced additions total **61,731,040 bytes (58.87 MiB)**, including the uniform,
terrain upload and probe buffers. The CPU-focused run records 7.77 / 10.36 ms of
total engine-process CPU time per frame for shore/marsh, across its threads. Fog
CPU preparation/recording/composition is approximately 0.105 ms and bloom recording
approximately 0.118 ms; these are separate from worker and other engine CPU work.
The CPU-focused run's GPU timings differ slightly with load but also pass all gates.

The measurements above describe the original month-one build, before the later
LUT, sky irradiance and cloud reconstruction additions. They are not performance
acceptance evidence for the expanded build. Physical lighting/exposure calibration
and full-scene 1080p/60 remain separate outstanding work.

## Transport and debugging notes

- **Extinction** is a nonnegative coefficient in m⁻¹. **Albedo** is bounded RGB
  scattering/extinction. **Optical depth** is extinction integrated over metres;
  transmittance is `exp(-opticalDepth)`.
- **Phase direction** is `dot(cameraToSample, sampleToEmitter)`. Positive HG
  anisotropy peaks when looking toward the light. No independent additive shaft
  brightness is composed after water.
- **Froxels** store source/extinction at cell centres. The integrated texture
  stores accumulated scattering/transmittance at slice boundaries, including
  boundary zero. A surface consumes only its final partial slice.
- The sky/aerial contribution remains separate spectral transport in month one.
  Water reads unfogged refraction input and receives camera transport once at its
  actual intersection. Reflected fog uses the analytic month-one approximation.
- Both histories are initialized. The volume uses an unjittered grid; opaque
  positions use the actual jittered surface projection. Scene TAA and fog history
  are separate systems. Cuts, projection/quality changes, diagnostics, terrain
  publication and sun/moon handoff reject fog history.
- `render.stats()` reports each fog pass, bloom, sky/environment, clouds, SSAO,
  CPU recording time and VMA image/buffer allocation totals. Temporary retired
  resources appear in totals during quality changes; measure budgets after warmup.
  `gpuWaterMs` now isolates the water pass; its previous broad group also contained
  the scene copy and opaque composition.
- Pin `fixedTime` and reset history for deterministic comparisons. Fixed time
  freezes animation; the repeatable eight-sample visibility/injection sequence
  still advances to make temporal noise reduction measurable.
- `terrain.brush_height(x,z,radius,strength,lower)` uses the editor's main-thread
  brush path. Immutable jobs publish only when their generation and camera region
  are current. Missing terrain falls back to global height fog.
- Settings loads migrate in memory, and saves emit nested schema version 2.
  Legacy dust extinction is `0.001 * volumetricDensityScale`; intensity/tint become
  bounded albedo. Appearance changes because this formerly additive dust now also
  attenuates surfaces. Explicit nested values override flat aliases in a request.
- Hidden lifetime tests set `GLGEN_PLAY_SNAPSHOT` to a private file. They do not
  overwrite the live editor's Play/Stop snapshot. The normal editor keeps its
  existing default path.

## Remaining staged roadmap

### Follow-up implementation and review

`captures/atmosphere_completion/asset_review` records an actual inspection of
74 current captures and ten previous/current/difference contact sheets. Thirty-eight
existing-asset shading images were accepted after review; their previous bytes
and SHA-256 hashes are retained in `previous_goldens` and `reviewed_goldens.json`.
All 38 reviewed images pass a fresh engine run in `coupled_validation`. Wrench/boulder
shading was reviewed separately; their legacy metric failures remain visible.
Eighteen newly added recipes still need content approval/initial references.
Examples requiring content repair include the exploded barrel and the virtual-city
recipe that renders as a cylinder. These are not atmosphere regressions.

The follow-up adds `SkyAtmosphere.h` and graphics-queue LUT generation in the
existing atmosphere helper: transmittance 256×64, isotropic multiple-scattering
32², sky view 192×108, and aerial perspective 32³. Separate RGB scattering and
RGB transmission images preserve spectral transport. The ozone tent spans
10–40 km with a peak at 25 km. Planet/profile calculations use kilometres to
avoid cancellation at the ground. Directional surface/fog lighting uses the same
profile through a double-precision CPU integral. The engine's existing light
scale is retained; physical lux/candela calibration is not yet complete.

Transmittance and multiple scattering rebuild on haze changes. Sky view rebuilds
on camera altitude and light changes. Aerial tables also rebuild for the unjittered
camera/projection. Both visible sky and environment faces sample the tables;
opaque/water camera transport samples spectral aerial data. Reflected water
transport retains its analytic approximation. `atmosphere.physicalSky` selects
the new path; atmosphere-off canonical turntables keep their analytic background.

The cloud-shadow addition uses a camera-centred 512² field over 8192 m, built
from the visible cloud density/weather/erosion functions and sampled by opaque
surfaces, water and directional fog lighting. Receivers project to the map's
y=0 plane; the map fades at its boundary. Clear/disabled cloud paths are neutral.
Cloud edits invalidate fog history; normal drift retains it. The field does not
replace the later dedicated cloud temporal reconstruction or near-camera medium.

`VulkanCloudHistory` subsequently adds independent half-resolution RGB
scattering/transmission histories with R32F scattering depth. Radiance-weighted
depth uses an opacity-weighted fallback in darkness. Reprojection accounts for
camera translation and bulk wind; detail/weather drift differences are bounded
by current-neighbour clipping and depth/opacity rejection. Both slots initialize
before use and retire behind existing frame fences. Cuts, projection changes,
cloud edits, time jumps, diagnostics and sun/moon handoff reject history.
Canonical asset reviews copy current cloud output with a pinned sampling phase.

Cloud marching now honours its sample budget, averages four stratified density
samples per segment and filters detail to the pixel/segment footprint. The
`quality_validation` run measures **91.4% less still-camera noise** after history
with scene TAA off, checks cuts, clearing and disabled copying, and passes the
combined transport/lighting/lifetime suite with zero synchronization errors.
Fixed and moving captures were inspected; this does not claim complete removal
of noise or the later near-camera cloud/fog medium.

The isolated cloud-quality review traced prominent comb-like opacity bands to
the legacy detail input. It has discontinuous borders and embedded text. The
upgraded path now uses an engine-owned periodic four-channel cellular volume,
baked with fixed integer lattice hashing (`Tools/bake_cloud_detail.py`). Canonical
reviews retain the legacy input. Provenance and SHA-256 accompany the new volume.
Repeatability and periodic neighbour checks pass; `periodic_cloud_review` shows
the dominant comb bands removed in the fixed view, cloud-history noise reduced
by 90.6% and zero synchronization errors. This is a reviewed improvement, not a
claim that the later cloud transport/phase work is complete. The new asset's GPU
mip-chain allocation is included in atmosphere totals and reported separately.

Sky irradiance uses nine real SH coefficients projected from the finished
environment. Lambert convolution supplies surface E/pi; the zeroth coefficient
supplies mean sky radiance for fog. RGB coefficients stay on the graphics queue,
without CPU readback. Six direct cosine integrals independently validate the SH
approximation: maximum error relative to the brightest reference is 2.15% at noon,
6.84% at hazy sunset and approximately 1.4% at twilight/night. Negative ringing
is clamped. Reference: [Ramamoorthi and Hanrahan](https://graphics.stanford.edu/papers/envmap/envmap.pdf).

Terrain basin potential uses [Priority-Flood](https://arxiv.org/abs/1511.04463)
on the immutable 256² height snapshot. Lowest spill heights form bounded cold-air
mist layers; open boundaries and missing data drain. The potential is encoded
in the existing field's validity channel and never changes terrain or water.
This is a geometric pooling model, not a fluid/weather simulation. Settings
expose enable, extinction and maximum depth; field generations and edits use
the existing publication path.
`valley_validation` measures actual GPU extinction 0.019989 m⁻¹ in an edited
closed basin and zero when pooling is disabled. Terrain regeneration, five
resizes, scenery/quality switches, Play/Stop and turntable restoration pass with
zero synchronization errors. Its asset report preserves exactly 36 missing
new-content goldens and the three legacy geometry differences; all reviewed
images match again.

`sky_validation/results.json` verifies all six LUT images are finite, the initial
aerial slice is vacuum, rebuild rules hold and sampled transmission differs from
independent 8192-step quadrature by at most **0.047% absolute**. Its noon, sunset,
twilight and night captures were inspected. `lut_lifetime/results.json` passes
transport, lights, terrain, five resizes, scenery/quality switching and Play/Stop
with zero synchronization errors. CPU tests: **256 cases / 482,775 assertions**.

The first wider LUT performance run (`lut_performance/results.json`) **fails**
the conservative fog-plus-whole-water gate: shore median 4.64 ms, p95 7.34 ms;
marsh median 2.16 ms, p95 5.59 ms. This failure remains recorded. The following
build caches view-projection matrices instead of inverting them in fog/water
fragments and adds cloud coupling. `coupled_performance` still fails under
shared GPU load (another game was running). The user explicitly prioritized
visual quality and deferred optimization. New cloud reconstruction allocations
are reported separately and also included in the atmosphere allocation total.
`gpuSkyLutsMs`, `gpuCloudShadowMs` and `gpuCloudHistoryMs` expose the additions.

Weeks 5–8 now include LUTs, ozone, projected cloud shadows, sky SH, dedicated
cloud history, basin mist and histogram metering. Physical lighting calibration and pre-exposure
and the weeks 9–20/research phases remain outstanding. The complete roadmap is
not yet implemented.

Histogram metering reads world HDR after scene TAA and before weapon, bloom and
UI. A 256-bin logarithmic histogram trims 5–95% of positive samples, excluding
black. The main thread consumes retired bins behind the existing frame fence;
adaptation interpolates in stops with a frame-rate-independent speed dial.
`exposure_quality_validation` verifies 0.125, 1 and 8 radiance inputs within the
6% quantization tolerance, bounded black input and stable manual exposure, then
passes resize, scenery, Play/Stop and turntable checks with zero synchronization
errors. Metering uses the existing radiance units; it does not establish absolute
photometry or pre-exposure.

The list below retains the roadmap dependencies. Re-estimate dates from measured
delivery throughput; implemented parts of weeks 5–8 are documented above.

1. **Complete weeks 5–8:** calibrated directional units and pre-exposure,
   followed by coupled visual/reference validation. LUTs, ozone, sky SH,
   histogram metering, 512² cloud transmittance shared by surfaces/fog,
   scattering-depth history and basin pooling are implemented. Validate against
   [Hillaire's reference implementation](https://github.com/sebh/UnrealEngineSkyAtmosphere).
2. **Weeks 9–12:** graphics-independent light/fog-volume ECS components;
   point/spot clustered lists; box/sphere/ellipsoid/cylinder volumes, density
   textures and emission; persistence, hierarchy, gizmos and Lua; weather curves;
   transparent transport and bounded emitter injection.
3. **Weeks 13–16:** normalized HG+Draine fit and sampling from the
   [authors' derivation](https://research.nvidia.com/labs/rtr/approximate-mie/);
   dense self-shadowing; shared near-camera cloud/fog density; cloud start beyond
   the fog range; near-field marching; underwater transport; deterministic offline
   volumetric reference renderer.
4. **Weeks 17–20:** PSF/FFT bloom, optional halation/streaks/lens ghosts;
   HDR display output; sparse authored-volume ingestion and scalability profiles.
5. **Time-boxed research:** ReSTIR volumes, physical lens tracing and learned
   multiple scattering. Shipping requires a measured benefit against the reference.

Hardware visibility queries remain primary; CSM/ESM, epipolar/radial shafts and
light cards are alternatives. AgX remains default. The integrated-system principle
follows the public [SIGGRAPH presentation](https://www.realtimerendering.com/advances/s2019/index.htm),
without assuming undocumented RDR2 internals.

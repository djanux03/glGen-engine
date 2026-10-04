# Asset regression

Catches unintended changes to generated content: a generator that started
emitting different geometry, a shader that changed how it looks, a material
that stopped binding.

```bash
GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe &

python Tools/glgen-regress/regress.py                  # check
python Tools/glgen-regress/regress.py --metrics-only   # fast; no rendering
python Tools/glgen-regress/regress.py --only tree      # one subset
python Tools/glgen-regress/regress.py --update         # accept the current output
python Tools/glgen-regress/regress.py --update-images  # accept shading only; preserve metrics
```

Exits non-zero on any difference, so it works as a CI gate.

## Unified atmosphere regression

`atmosphere.py` starts its own hidden engine and private command port at verified
1920×1080. It preserves settings and goldens. Run validation separately from GPU
performance measurements:

```powershell
python Tools/glgen-regress/atmosphere.py --sync-validation --light-check --terrain-check --output captures/atmosphere_transport
python Tools/glgen-regress/atmosphere.py --sync-validation --performance-only --lifecycle-check --graphics-check --output captures/atmosphere_lifetime
python Tools/glgen-regress/atmosphere.py --performance-only --light-check --visual-review --asset-regression --output captures/atmosphere_review
python Tools/glgen-regress/atmosphere.py --sync-validation --performance-only --sky-check --output captures/sky_luts
python Tools/glgen-regress/atmosphere.py --sync-validation --performance-only --cloud-shadow-check --cloud-history-check --output captures/cloud_transport
python Tools/glgen-regress/atmosphere.py --sync-validation --performance-only --exposure-check --cloud-quality-review --output captures/exposure_cloud_review
python Tools/glgen-regress/atmosphere.py --sync-validation --performance-only --swamp-only --visual-review --cloud-quality-review --output captures/swamp_review
```

Transport checks read actual RGBA16F GPU values after the normal frame fence:
homogeneous media, height profiles, partial opaque slices, actual water hits and
uniform bloom. Light checks cover the phase direction, finite shadows, imported
alpha cutouts, independent history and moving-point bloom. Terrain checks edit
the private world's height and verify field publication and regeneration.
Lifecycle checks create a private player, resize without activation, switch
scenery/quality, enter/leave play and verify turntable state restoration. A private
`GLGEN_PLAY_SNAPSHOT` path keeps the live editor's snapshot untouched. Captures still need visual
review; a passing numeric suite does not approve new goldens.

`--sky-check` reads the complete transmittance, multiple-scattering, sky-view
and RGB aerial tables, checks finite/bounded transmission, compares sixteen
transmission texels to independent dense quadrature at three haze levels, and
checks rebuild rules. It captures clear noon, hazy sunset, twilight and night.
`gpuSkyLutsMs` measures LUT generation separately from sky/environment drawing.
`atmosphere.physicalSky` selects the LUT path; canonical atmosphere-off asset
turntables retain the analytic background for stable material comparisons.

The sky check also compares nine-coefficient Lambert irradiance against direct
GPU cosine quadrature for six normals in each lighting scene. Surface diffuse
lighting and fog ambient use the same rendered sky/cloud environment. Reference:
[Ramamoorthi and Hanrahan](https://graphics.stanford.edu/papers/envmap/envmap.pdf).

`--cloud-shadow-check` verifies clear/overcast projected transmission and its
effect on actual surface and fog lighting. `--cloud-history-check` compares raw
and reconstructed floating-point cloud samples with scene TAA disabled, checks
cuts, wind/camera movement, clearing the deck and disabling reconstruction.
Cloud history has separate scattering depth, descriptors and initialized slots.
`gpuCloudHistoryMs` and `cloudHistoryAllocatedBytes` show its cost; the latter is
also included in the total `atmosphereAllocatedBytes`, including retired grids.

`--exposure-check` meters known world radiances through the GPU histogram,
checks bounded black input and manual exposure, and restores the previous state.
The 256-bin meter excludes black and trims 5–95% of the positive population.
It runs before weapon, bloom and UI; readback uses the existing frame fence.
`gpuExposureMeterMs` and `meteredLuminance` report it. This is radiance metering;
photometric calibration and pre-exposure are separate follow-up work.

`--cloud-quality-review` captures the same view with history, detail, curl,
self-shadowing and sample budgets isolated. Inspect these images when diagnosing
banding; it never accepts references. The upgraded path uses engine-owned
periodic cellular detail baked by `Tools/bake_cloud_detail.py`; legacy canonical
reviews retain their established texture. `cloudDetailAllocatedBytes` includes
the new asset's mip-chain allocation in the total atmosphere budget.
`--swamp-only` keeps visual and lifecycle review in `woodland_swamp`, including
repeated reloads of that map instead of switching to winter or meadows.

`regress.py --report path.json` records per-recipe metric failures, image
comparisons and capture SHA-256 hashes. This keeps unrelated content/geometry
failures visible when reviewed shading references are accepted.

## Two signals, because they fail differently

**Metrics** — triangle count, vertex count, submesh count, bounds. Cheap,
exact, and the diff names the number that moved:

```
tree/oak_broad: triangles 2712 -> 2946
```

They cannot see anything about shading.

**Images** — a turntable per recipe, compared pixel by pixel. Catches what
metrics structurally cannot. Demonstrated by recolouring a recipe and
re-running: metrics passed (geometry was untouched) while both images failed at
~7% of pixels.

## Why the renders are reproducible

They were not, at first. Three things had to be pinned, and each was found by
re-running an unchanged check and watching it fail:

- **The animation clock.** Cloud drift, dew twinkle and volumetric turbulence
  all key off elapsed time. `VulkanRenderer::Params::fixedTimeSeconds` pins it;
  the turntable sets it automatically.
- **The world in the background.** At the original +55 m the streaming terrain,
  settling physics bodies and scattered vegetation were all in frame, and all
  three differ between runs. The turntable now lifts the subject **past the far
  plane**, so the world is simply not drawn.
- **The camera grade.** It eases toward the biome under the camera on the real
  clock, so it landed somewhere slightly different every run. Disabled for the
  duration of a turntable.

With those fixed, an unchanged re-run reports a mean per-pixel delta of `0.00`.
The tolerance is deliberately tight (4/255 per channel, 0.2% of pixels) — a
loose tolerance is how a regression check quietly stops working.

## Maintaining the goldens

`Tests/golden/` holds 12 PNGs and `metrics.json`, about 2 MB. Re-run with
`--update` when a change is intended, and **look at the diff** before accepting
it — that is the whole point of the check.

No third-party dependencies: the PNG reader uses stdlib `zlib`, which handles
the non-interlaced 8-bit files `stb_image_write` produces.

## Woodland ground review

`python Tools/glgen-regress/ground_review.py --output captures/ground_review`
starts a separate hidden woodland runtime with a pinned clock and captures
the same gravel verge, grass, litter and walking-route views on each run.
It records GPU timings and drawn vegetation triangles in `results.json`.
Use separate output folders for before/after comparisons; inspect the PNGs.
`--asset-regression` runs the full recipe check through that same process and
records its exit code without accepting or overwriting existing goldens.

Rebuild the mixed meadow with `python Tools/glgen-trees/make_meadow_variants.py`.
`install_meadow_variants.py` applies its stock layers to shipped and saved profiles
while preserving older layer indices and landmark placement.
`make_terrain_dressing.py` bakes the shared broken sticks and leaf scraps along
with other dressing. Five shared grass meshes supply fine blades, broad leaves,
pale flowering stalks, dry stems and low cover. Near meshes use 560–672 triangles;
middle/far meshes use 54–72 and 18–24, switching at 12m and 40m.
Eight gradient variants in each of three colour families give leaves dark roots,
olive bodies and pale tips. Curved leaf geometry supplies the silhouettes without
alpha overdraw. All LODs share import bounds to keep their roots registered.
Grass placement uses bounded deterministic dart throwing rather than a jittered
one-plant-per-cell grid. Stock grass has blade shadow casting disabled, both in
the layers and the master setting. Full-resolution SSAO resolves small root and
clump contacts from the actual depth prepass. A small bilateral filter preserves
those pockets, and terrain's placement mask gates stronger contact occlusion.
Clear ground between roots retains its ordinary lighting; the coverage mask
alone no longer darkens the soil. The enhancement fades with grass draw distance and
adds no grass instances to the shadow acceleration structure. Grass still receives
tree and rock shadows and retains its near-camera edge softness.
`python Tools/glgen-regress/meadow_review.py --asset-regression` captures three
meadow views plus a ground-contact on/off comparison and an AO diagnostic with fixed time/exposure.
`--shadow-check` verifies stock layers add no grass casters even if the master
switch is turned on. `--ao-check` captures an
identical ground view with SSAO off/on and checks that contact shading darkens it.
SSAO uses a 24-sample contact/clump kernel with a 4mm bias and woodland strength 2.5.
Debris uses shared meshes
and finite draw distances; sticks cast too. Small scraps can lie in the road,
while grass fades across the shoulder and stays out of the compacted centre.

`--profile alpine_flyby --coverage-only` checks four high/middle/walking views
in the alpine preset as well. The hidden review process caps streaming at eight
chunks for these local views, keeping upload activity out of the comparison;
the authored preset retains its full streaming distance.

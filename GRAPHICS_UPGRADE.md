# Graphics upgrade

The Vulkan renderer now uses height-correlated Smith GGX visibility and normal
variance filtering for glossy materials. Sun and local lights share the same
material response. Point-light shadows stop at the emitter and respect foliage
opacity micromaps; backlit leaf transmission also respects visibility. Water
glints and volumetric shafts now use those same foliage gaps.

Terrain materials accept two additional fields:

```json
{
  "height": "materials/woodland_ground_8k/textures/woodland_gravel_height.jpg",
  "reliefDepth": 0.055
}
```

Height maps are linear data. `reliefDepth` is in metres, clamped to 0–0.2.
The near-field shader uses a bounded 12-step parallax march, blends material
transitions using height, and estimates micro-occlusion from height instead of
albedo darkness. Parallax fades between 10 and 38 metres and at steep slopes
and grazing angles. It does not displace geometry or alter collision/depth.
Missing height maps fall back to flat relief. The existing 2K woodland maps and
the shipped meadow/alpine material sets are connected; no upscaled source art
is represented as newly acquired photographic detail.

The terrain editor exposes base colour, normal, roughness, height and relief
depth. Scenery snapshots serialize the new fields; paths are resolved against
the asset directory. The GPU fields are appended to the shared frame UBO, with
matching C++/GLSL layout and static offset/size assertions.

Height fog integrates the actual density profile: exponential above its
reference altitude, constant below. The integral is reciprocal and avoids
positive exponent overflow or cancellation for near-horizontal rays. Water
uses wavelength-dependent absorption, shadowed scattering/glints, dispersed
wind waves and pixel-footprint filtering. Refraction rejection tests the actual
water hit, including lake height fields.

Temporal AA uses variance clipping and a luminance-responsive history weight.
`temporalSharpness` controls bounded detail recovery after the resolve (0–0.5;
default 0.22). It is available in the editor, scenery JSON, `render.params` and
the command port. Diagnostics bypass it. The woodland profile keeps fuller
tree LODs for longer, smaller ground texture repeats, quieter lake ripples and
less uniform haze. The other shipped/saved profiles now connect their existing
material maps and enable temporal AA.

`render.scenery(id)` and `render.scenery_id()` expose runtime scenery switching
to Lua. Asset turntables now produce exactly the requested view count, allow
eight settling frames per angle, use reproducible spatial AA and restore the lighting/fog/AA they temporarily
change. Previously the first angle was numbered -1 and left review lighting
in the world.

## Verification

```powershell
cmake --build Build-vs18 --config Release --target glGenVk
cmake --build Build-tests --config Release
Build-tests/Release/glgen_tests.exe
python Tools/glgen-regress/woodland_perf.py --visual-review --terrain-review --temporal-check --sync-validation --graphics-check --output captures/graphics_upgrade/verify
```

`graphics_checks.py` tests the actual GPU output for fog reciprocity across its
floor, non-finite extreme fog, local-light occlusion, finite shadow-ray range,
sharpness bounds and turntable state restoration. It can also run against a
private command port. `lighting.py` exercises meadow, sunset, night, winter,
metal/roughness/emission and local-light material charts. Its NaN check now
recognizes the shader's magenta diagnostic.

On the local Release build, all 237 doctest cases and 203,523 assertions pass.
The GPU fog/local-light checks and terrain-enabled lighting/material checks
pass. The final synchronization-validation run reports zero Vulkan errors,
preserves live terrain through scene snapshots and verifies hidden resize to
960×540. Temporal AA reduces measured consecutive-frame cloud noise by about
50% (0.570 to 0.282 mean channel codes). Its report is in
`captures/graphics_upgrade/validation_final/results.json`.
Twelve reviewed images for the six original core recipes repeat exactly
with spatial review AA; geometry metrics were kept unchanged. Existing metric
differences remain visible for `imported/wrench` (vertex count) and `rock/boulder`
(bounds), rather than accepting unrelated geometry changes with shading goldens.

Fixed woodland views at 1280×720 measured roughly 16–18 ms GPU time versus
12–14 ms before this upgrade on this machine. Fuller tree LODs, relief mapping
and additional visibility queries cost GPU time. This is a quality upgrade,
not a performance improvement. Captures are in `captures/graphics_upgrade/verified`;
build, regression and lighting logs are under `captures/graphics_upgrade`.

The upgrade retains screen-space reflections and a sky environment for misses;
it does not add scene-wide ray-traced reflections or global illumination. Water
waves still change normals on the existing water height field. Temporal history
has camera reprojection and clipping, but no per-object motion-vector stream.

Shading references: [Filament's physically based model](https://google.github.io/filament/Filament.html)
and [specular antialiasing controls](https://google.github.io/filament/Materials.md.html).

## Terrain dressing follow-up

The terrain now includes multi-stem leafy shrubs, low scrub, curved fern
patches, pebble beds and forest-floor deadwood. Fractured boulders, low slabs
and upright crags replace the single rounded rock silhouette. Shipped profiles
and saved profiles using known legacy art receive the new dressing; the woodland
also gains authored foreground plant pockets and varied rock-rise landmarks.
Legacy grass tufts and winter tree proxies now use the existing detailed meshes.

The new meshes share materials and instanced geometry, with two cheaper LODs.
Dense small dressing stays outside the ray-tracing instance list. Large rocks
and deadwood can opt into `avoidTracks`; clustered rock satellites now obey
their own waterline, habitat, spacing and chunk ownership checks. Fixed authored
landmarks retain their explicit coordinates.

The asset baker, triangle budgets and source texture references are documented
in `assets/terrain_dressing/README.md`. Eight new import recipes expose these
assets to turntable review. Captures and validation logs are in
`captures/terrain_upgrade`.

This pass builds successfully and passes all 238 doctest cases (273,399
assertions). All eight new recipes and sixteen reviewed images repeat exactly;
original recipe images still match in their canonical empty review environment.
The pre-existing wrench vertex-count and boulder-bound differences remain
unchanged. Terrain-enabled and snowy-profile runs report zero Vulkan validation
errors; fog and local-light GPU checks pass. Representative woodland views
measured about 17 ms GPU time at 1280×720 with synchronization validation enabled
on the local RTX 3070. This is a measurement of this run, not a promise of equal
cost on another scene or machine.

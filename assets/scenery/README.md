# Scenery presets

Choose **Terrain > Scenery > Bleak Winter > Apply Scenery**. If there is no
terrain, this stages the preset; **Create Terrain** generates it. Applying to
existing terrain regenerates terrain, scatter, collision, and water. Like
ordinary regeneration, it resets height-brush edits. Switching keeps the seed
and world boundaries. Meadows remains the default.

**Save All Settings** or **Save Scene** saves the selected scenery and edits to
both profiles in `assets/settings/scenery_settings.json`. The version-1 authored
definitions here remain separate. Winter never overwrites `terrain_scatter.json`.
Startup restores staged settings without generating terrain automatically.

Bleak Winter uses an illustrative winter palette inspired by *The Long Dark*:
near-continuous matte snow, cool blue shadows, subdued evergreen silhouettes,
and broad cloud washes. Snow variation is world-anchored at drift scale instead
of fine glitter. Rock/soil normal maps are omitted in this preset to keep the
large color shapes legible. The outline pass detects depth discontinuities
instead of darkening every face that points away from the camera.

**Environment > Volumetric Clouds > Painted Clouds** selects the smooth cloud
deck (enabled by winter); disable it to return to the volumetric layer. It uses
the same analytic deck as environment lighting, with no added texture assets or
raymarch. This option is saved with style/scenery settings and can also be set
with `render.params{paintedClouds=true}`. Saved custom scenery profiles still
override the shipped definition.

The five base material roles remain ground cover, forest floor, mud, rock, and
scree. Material key `5` is the optional snow texture set. Definition paths are
relative to `assets`; missing maps use the procedural material. Snow is a
surface overlay with unchanged geometry/collision. Scatter layers opt in with
`receivesSnow`; grass and ordinary scene objects remain uncoated. This version
has no snowfall, snow deformation, or frozen-water physics.

## Headless verification

```powershell
$env:GLGEN_SCENERY = 'bleak_winter'
$env:GLGEN_TERRAIN_ON_START = '1'
$env:GLGEN_FIXED_TIME = '100'
$env:GLGEN_VK_VIEWDIST = '6'
$env:GLGEN_AGENT_PORT = '8788'
./Build-vs18/bin/Release/glGenVk.exe
# In another shell:
python Tools/glgen-regress/scenery.py --port 8788
```

The harness writes five winter views, Meadows before/after switching, and
instance/frame-throughput measurements to `captures/winter`. It waits for
streaming and does not accept goldens. Inspect images before accepting visual
changes. Throughput includes the whole runtime, not isolated GPU time.

Lua/JSON-RPC uses the same main-thread path: `render.scenery("bleak_winter")`,
`render.scenery_id()`, `render.params{snowCoverage=0.5}`, and
`render.save_scenery()`. `GLGEN_SCENERY_SETTINGS` optionally selects a separate
profile file for tests. Explicit `GLGEN_SMOKE_CAM` still overrides the startup
camera; `GLGEN_FIXED_TIME` pins cloud, water, wind and shader animation time.

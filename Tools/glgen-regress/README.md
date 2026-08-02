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
```

Exits non-zero on any difference, so it works as a CI gate.

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

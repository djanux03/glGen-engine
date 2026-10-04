# Woodland ground materials

Five seamless, non-metallic PBR terrain materials made for the authored
woodland surface in `assets/scenery/woodland_swamp.json`:

| Slot | Surface | Runtime maps |
| --- | --- | --- |
| 0 | Woodland moss | Base color, tangent normal, roughness, height |
| 1 | Forest leaf litter | Base color, tangent normal, roughness, height |
| 2 | Compacted woodland gravel | Base color, tangent normal, roughness, height |
| 3 | Exposed forest bedrock | Base color, tangent normal, roughness, height |
| 4 | Marsh wet soil | Base color, tangent normal, roughness, height |

The woodland profile uses the 2K files in `textures/`. The 8K pixel-resolution
masters are in `masters_8k/` for offline use and export. Their source artwork is
1254x1254; the 8K files are Lanczos upscales and do not contain native 8K source
detail. At runtime the engine uploads uncompressed RGBA textures, so using all
fifteen 8K color/normal/roughness maps would take about 4 GiB of texture memory.
The 2K maps keep the profile practical while preserving the source detail.

The generated color tiles were blended across a 12% border band to make their
opposite edges repeat. Normal and height maps are derived from local albedo
luminance after broad shading is removed. Roughness values are authored per
surface and modulated by local detail. These are useful game-ready material
maps, not independent photogrammetry scans. There is no metallic map because
these are dielectric ground surfaces. The terrain shader uses height for near-field parallax relief, material
transitions and micro-occlusion. `reliefDepth` in the scenery material slot
sets its depth in metres; the effect fades at distance and steep view angles.

`materials.json` lists runtime paths, 8K master paths, source tiles, and the
recommended world-space tiling values. To rebuild maps after replacing a source
tile, run from the repository root:

```powershell
python Tools/glgen-materials/build_woodland_pbr.py
```

The builder requires Pillow and NumPy. The existing woodland profile is already
configured to use the runtime maps.

## Artwork prompts

Each source is a top-down, evenly lit, seamless albedo tile without text or
perspective. The five surface descriptions used for generation were:

- Dense emerald/olive moss cushions with tiny soil pores and sparse leaf flecks.
- Small decaying birch/beech leaf pieces, spruce needles, bark flakes, and dark
  humus; no oversized intact leaves.
- Compacted track gravel made of small rounded and angular granite, charcoal,
  ochre, and gray pebbles embedded in mineral fines.
- Fine-grain slate and weathered granite bedrock with small facets, hairline
  fissures, mica specks, and no oversized dramatic cracks.
- Dark saturated marsh loam with small crumbs, fine roots, organic debris, and
  no standing water or baked reflections.

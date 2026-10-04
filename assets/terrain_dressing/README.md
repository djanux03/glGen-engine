# Terrain dressing

Original, deterministic shared meshes baked by
`Tools/glgen-trees/make_terrain_dressing.py`. Geometry is authored in metres,
Y up. Twenty-two files cover eight assets and their render detail levels.

| Asset | Full triangles | Mid | Far | Role |
|---|---:|---:|---:|---|
| Hazel bush | 370 | 110 | 30 | Multi-stem, irregular leafy understory |
| Low scrub | 518 | 154 | 42 | Lower bank and mountain vegetation |
| Woodland fern | 756 | 240 | 54 | Arched fronds with paired pinnae and rachises |
| Fractured granite | 512 | 128 | 32 | Weathered boulders with coherent fracture planes |
| Granite slab | 512 | 128 | 32 | Low, wide broken rock |
| Granite crag | 512 | 128 | 32 | Upright outcrop silhouette |
| Pebble bed | 224 | 64 | 8 | Several small stones in one shared instance |
| Forest deadwood | 74 | — | — | Fallen twigs and woody ground debris |

Texture sources already present in the repository:

- Shrubs reuse `assets/trees/birch/birch_spray_v2.png`; its source record is
  `assets/trees/birch/texture_provenance.json`. Material alpha masks also feed
  depth testing and ray opacity micromaps.
- Granite uses `assets/materials/m1/textures/stone-47_diffuse.jpg` and
  `stone-47_normals.jpg`, with world-scale planar UVs to avoid stretched poles.
- Ferns use the original procedural blade-gradient function from
  `make_woodland_grass.py`, embedded in glTF. Deadwood uses rough matte wood.

`manifest.json` records triangle counts and SHA-256 hashes. The baker uses an
explicit integer hash and writes identical bytes when repeated.

`dress_terrain_profiles.py` connects these files to shared, shipped and saved
scenery. It replaces only known legacy tree, grass and rock art, appends new
layer names without shifting existing layer seeds, and can be repeated without
duplicating layers. The woodland's fixed outcrop coordinates are retained but
split among three rock silhouettes; small foreground plant pockets complement
the procedural population.

Woody shrubs use Tree layers, so tree density/size controls also affect shrubs.
Ferns use Grass layers. Ferns, pebbles and deadwood stay outside the TLAS;
larger rocks and shrubs cast ray shadows. Small culling cells and two cheaper
detail levels limit the added cost. Large rock layers use the engine's existing
analytic collision proxy; these are not exact convex mesh colliders.

`avoidTracks` is an optional persisted scatter field for rocks and deadwood.
It leaves authored service tracks clear while permitting small road gravel.
Fixed placements remain explicit. Rock satellites now honor their own biome,
spacing, slope, moisture, waterline, track and half-open chunk boundaries.

Review command:

```powershell
python Tools/glgen-regress/woodland_perf.py --visual-review --terrain-review --dressing-review --sync-validation --output captures/terrain_dressing
```

The eight `terrain/` import recipes support ordinary turntable regression:
`python Tools/glgen-regress/regress.py --only terrain/`.

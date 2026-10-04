# Woodland & Swamp surfaces

Native 4096×4096 Poly Haven photographs and PBR maps, downloaded unchanged.
These replace the upscaled generated tiles in this scenery. They are used by
`assets/scenery/woodland_swamp.json`; other scenery definitions keep their assets.

| Slot | Surface | Source |
| --- | --- | --- |
| 0 | Moss, small sticks and organic cover | [Forest Leaves 02](https://polyhaven.com/a/forest_leaves_02) |
| 1 | Fallen leaves and granular forest soil | [Forest Floor](https://polyhaven.com/a/forest_floor) |
| 2 | Gravel and worn service tracks | [Forest Ground 04](https://polyhaven.com/a/forest_ground_04) |
| 3 | Weathered bedrock | [Rock Face 03](https://polyhaven.com/a/rock_face_03) |
| 4 | Damp soil with leaf debris | [Brown Mud Leaves 01](https://polyhaven.com/a/brown_mud_leaves_01) |

Each surface includes diffuse colour, OpenGL tangent normals, roughness and
height. Tile sizes are metres. Bounded parallax supplies near-ground relief;
height blending and narrow anti-tiling seams retain individual stones/leaves.

Grass is conditioned from [Grass Medium 01](https://polyhaven.com/a/grass_medium_01)
and [Grass Medium 02](https://polyhaven.com/a/grass_medium_02). The published alpha
is merged into diffuse PNGs because their downloadable JPEG glTF loses alpha.
The photographed blade gradients and original UVs are retained. Complete leaf
subsets supply LODs and sparse shadows, with identical bounds for stable roots.

All source assets are CC0. Authors, exact URLs and source MD5s are recorded in
`provenance.json`. Reproduce and verify downloads, geometry and RGBA atlases:

```powershell
python Tools/glgen-trees/condition_woodland_scans.py --fetch
```

The shader changes preserve scanned grass hues in this profile, soften its
normal detail and diffuse response, and reduce grazing cuticle highlights.
Grass shadows stay enabled with partial clump coverage; SSAO strength is 2.5.

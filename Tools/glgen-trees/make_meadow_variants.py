"""Bake five shared grass silhouettes inspired by a mixed, sunlit wild meadow.

These are mesh variants, not a mesh per instance. Stable integer hashes and
common import bounds preserve reproducibility and root registration across LODs.
Ground contact comes from the terrain patch mask; no casting meshes are needed.
"""
import json
from make_woodland_grass import ROOT, bake

VARIANTS = [
    ('meadow_fine', .95, .62, 48, 71011,
     dict(width_scale=.75, green_fraction=.76, straw_fraction=.08, head_every=0)),
    ('meadow_broad', .65, .66, 42, 72019,
     dict(width_scale=1.45, lean_scale=1.15, green_fraction=.70, head_every=0)),
    ('meadow_seedheads', 1.30, .52, 36, 73009,
     dict(width_scale=.85, head_every=8, green_fraction=.68, straw_fraction=.20)),
    ('meadow_dry', .90, .58, 40, 74017,
     dict(width_scale=.85, lean_scale=1.25, green_fraction=.45, straw_fraction=.42, head_every=16)),
    ('meadow_low', .28, .60, 40, 75011,
     dict(width_scale=1.2, lean_scale=1.5, green_fraction=.80, straw_fraction=.05, head_every=0)),
]

def main():
    specs=[]
    for name,height,spread,blades,seed,options in VARIANTS:
        bounds=bake(name,height,spread,blades,seed,bands=4,**options)
        bake(name+'_mid',height,spread,blades,seed,stride=2,bands=2,bounds=bounds,**options)
        # Actual curved blades keep each species' colour and silhouette at
        # distance. A common photo card would erase the differences between them.
        bake(name+'_far',height,spread,blades,seed,stride=6,bands=2,bounds=bounds,**options)
        specs.append(dict(name=name,height=height,spread=spread,blades=blades,
                          seed=seed,options=options,castRayShadow=False))
    (ROOT/'assets/grass/meadow_variants.json').write_text(json.dumps(
        dict(description='Fine, broad, flowering, dry and low meadow grass; shared LODs, no blade shadows.',
             variants=specs),indent=2)+'\n')

if __name__=='__main__': main()

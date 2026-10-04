"""Install the mixed meadow into stock/saved profiles without moving landmarks.

Only the known stock grass layers change mesh. Append extra variants to preserve
all older layer indices/seeds. Seasonal density and custom layers stay authored.
The grass master switch disables all grass blade casting, including custom meshes.
"""
import copy
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]

def mesh(row,name):
    row['mesh']=f'assets/grass/{name}.gltf'
    row['meshLods']=[dict(mesh=f'assets/grass/{name}_mid.gltf',distance=12),
                     dict(mesh=f'assets/grass/{name}_far.gltf',distance=40)]
    row.pop('shadowMesh',None)
    row.update(meshUpAxisFixDeg=[0,0,0],castRayShadow=False,alphaCutout=False,
               tint=[1,1,1],groundOcclusion=.38)

def install(profile):
    terrain=profile.setdefault('terrain',{})
    terrain['grassCastShadows']=False
    manifest=profile.get('scatter',profile)
    rows=manifest.get('layers',[])
    for row in rows:
        if row.get('type')=='grass': row['castRayShadow']=False
    main=next((r for r in rows if r.get('name') in ('grass_meadow','woodland_grass')),None)
    if main is None or main.get('mesh','').endswith('meadow_fine.gltf'): return
    if main.get('mesh') not in ('assets/grass/woodland_grass.gltf','assets/grass/woodland_scan/meadow.gltf'): return
    seasonal=main['density']/(2.3 if 'woodland_scan' in main['mesh'] else 1.7)
    mesh(main,'meadow_fine')
    main.update(density=1.3*seasonal,scale=[.85,1.15],heightScale=[.78,1.18],
                sinkIntoGround=.015,windStrength=.045,cullCellSize=3,
                maxDrawDistance=115,densityFalloffStart=85)
    for row in rows:
        if row.get('name') in ('grass_tuft','woodland_flowering_grass'):
            mesh(row,'meadow_seedheads')
            row.update(density=.28*seasonal,heightScale=[.8,1.15],scale=[.85,1.15],
                       sinkIntoGround=.015,cullCellSize=3)
        elif row.get('name')=='grass_low_sward':
            mesh(row,'meadow_low')
            row.update(density=max(row['density'],1.0*seasonal),scale=[.85,1.2],
                       heightScale=[.8,1.2],sinkIntoGround=.015)
    for name,asset,density,patch in [('grass_broad_leaf','meadow_broad',.8,1.8),
                                    ('grass_dry_stems','meadow_dry',.5,2.1)]:
        extra=copy.deepcopy(main)
        mesh(extra,asset)
        extra.pop('fixedPlacements',None)
        extra.update(name=name,density=density*seasonal,patchScale=patch)
        if not any(r['name']==name for r in rows): rows.append(extra)

def main():
    paths=[ROOT/'terrain_scatter.json',*sorted((ROOT/'assets/scenery').glob('*.json')),
           ROOT/'assets/settings/scenery_settings.json']
    for path in paths:
        doc=json.loads(path.read_text())
        if 'profiles' in doc:
            for profile in doc['profiles'].values(): install(profile)
        else: install(doc)
        # A top-level terrain block does not belong in the standalone manifest.
        if path.name=='terrain_scatter.json': doc.pop('terrain',None)
        path.write_text(json.dumps(doc,indent=2)+'\n')
        print(path.relative_to(ROOT))

if __name__=='__main__': main()

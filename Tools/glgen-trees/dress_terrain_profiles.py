"""Connect shared terrain dressing to shipped and saved scenery profiles.

Only known legacy art is replaced. Authored/custom meshes, densities and
landmarks remain editable. New layers are appended so existing layer seeds
and fixed landmark ownership do not shift. Re-running never duplicates them.
"""
import copy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT/'assets'


def lods(name, distances):
    return [{'mesh': f'assets/terrain_dressing/{name}_{suffix}.gltf', 'distance': distance}
            for suffix,distance in zip(('mid','far'), distances)]


def layer(name, mesh, kind, density, biomes, scale, distance=180, **fields):
    result = {'name': name, 'mesh': f'assets/terrain_dressing/{mesh}.gltf',
        'type': kind, 'density': density, 'biomes': dict(zip(('meadow','forest','mountain'),biomes)),
        'scale': scale, 'heightScale': [.88,1.12], 'meshUpAxisFixDeg': [0,0,0],
        'randomYaw': True, 'alignToNormal': .15, 'slopeMax': .55,
        'minSpacing': 1.5, 'moistureMin': 0, 'sinkIntoGround': .035,
        'collision': 'none', 'interactive': False, 'castRayShadow': kind in ('tree','grass'),
        'receivesSnow': kind != 'grass',
        'avoidTracks': kind == 'rock' and mesh != 'pebble_bed',
        'cullCellSize': 8, 'maxDrawDistance': distance, 'densityFalloffStart': distance*.75,
        'tint': [1,1,1], 'groundOcclusion': .25, 'wind': kind != 'rock',
        'windStrength': .09 if kind == 'tree' else .055, 'windSpeed': .65,
        'alphaCutout': kind == 'tree', 'foliageSssStrength': .7 if kind == 'tree' else (.5 if kind == 'grass' else 0),
        'patchScale': 1.2, 'patchThreshold': .43,
        'clustering': {'stands': False, 'outcrops': False, 'clearingChance': .15, 'standRadius': 12}}
    result.update(fields)
    return result


def dressing(cold=False):
    shrubs = .55 if cold else 1
    return [
        layer('hazel_understory','hazel_bush','tree',.055*shrubs,(.3,1,.08),[.8,1.5],240,
              meshLods=lods('hazel_bush',(32,90)),minSpacing=2.3),
        layer('low_bank_scrub','low_scrub','tree',.055,(.55,.4,.5),[.85,1.5],210,
              meshLods=lods('low_scrub',(28,75)),minSpacing=2.4,patchThreshold=.48),
        layer('woodland_fern_patches','woodland_fern','grass',.26 if not cold else .08,
              (.08,1,.04),[.55,1.1],110,minSpacing=.65,moistureMin=.35,
              meshLods=lods('woodland_fern',(38,75)),alphaCutout=False,groundOcclusion=.32),
        layer('small_stone_beds','pebble_bed','rock',.08,(.7,.45,1),[.7,1.6],90,
              meshLods=lods('pebble_bed',(35,65)),minSpacing=1.3,alignToNormal=.8,
              patchThreshold=.54,slopeMax=.85,sinkIntoGround=.005,wind=False),
        layer('fractured_rock_slabs','granite_slab','rock',.0018,(.25,.5,1),[.55,1.4],360,
              meshLods=lods('granite_slab',(65,180)),minSpacing=5,
              collision='convex-or-sphere',
              castRayShadow=True,alignToNormal=.55,slopeMax=.85,sinkIntoGround=.12,
              clustering={'stands':False,'outcrops':True,'clearingChance':0,'standRadius':25}),
        layer('fractured_rock_crags','granite_crag','rock',.0009,(.1,.45,1),[.55,1.25],400,
              meshLods=lods('granite_crag',(65,180)),minSpacing=7,
              collision='convex-or-sphere',
              castRayShadow=True,alignToNormal=.25,slopeMax=.8,sinkIntoGround=.15),
        layer('forest_floor_deadwood','forest_deadwood','rock',.022,(.06,.85,.03),[.8,1.4],120,
              minSpacing=2.1,alignToNormal=.8,sinkIntoGround=.02,
              patchThreshold=.4,wind=False)]


def ground_dressing():
    return [
        layer('broken_verge_sticks','broken_ground_sticks','rock',.34,(.8,1,.12),[.65,1.15],65,
              minSpacing=.8,alignToNormal=1,sinkIntoGround=.006,patchScale=0,
              avoidTracks=False,castRayShadow=True,wind=False,cullCellSize=6),
        layer('fallen_leaf_scraps','curled_leaf_litter','rock',.25,(.45,1,.05),[.65,1.1],45,
              minSpacing=.65,alignToNormal=1,sinkIntoGround=0,patchScale=0,
              avoidTracks=False,castRayShadow=False,wind=False,cullCellSize=6)]


def upgrade(manifest, cold=False, woodland=False):
    result = copy.deepcopy(manifest)
    result['version'] = 6
    for row in result['layers']:
        mesh = row.get('mesh','').replace('\\','/')
        if mesh in ('assets/terraingeneratorassets/rock.obj','assets/woodland_props/granite_boulder.gltf'):
            row.update(mesh='assets/terrain_dressing/fractured_granite.gltf',meshUpAxisFixDeg=[0,0,0],
                       meshLods=lods('fractured_granite',(65,180)),cullCellSize=16,avoidTracks=True)
            if not row.get('fixedPlacements'): row['sinkIntoGround'] = .08
            if row.get('maxDrawDistance',1e6) >= 1e6: row['maxDrawDistance']=440
        if mesh == 'assets/terraingeneratorassets/tree.obj':
            row.update(mesh='assets/trees/spruce/spruce_dense.gltf',meshUpAxisFixDeg=[0,0,0])
            row['scale'] = [.35,.7] if 'young' in row['name'] else [1.25,2.15]
        if mesh in ('assets/terraingeneratorassets/grass.obj','assets/grass/grasspatch.gltf'):
            row.update(mesh='assets/grass/woodland_grass.gltf',meshUpAxisFixDeg=[0,0,0],
                scale=[.65,.95],cullCellSize=8,maxDrawDistance=110,densityFalloffStart=80,
                meshLods=[{'mesh':'assets/grass/woodland_grass_mid.gltf','distance':28},
                          {'mesh':'assets/grass/woodland_grass_far.gltf','distance':65}],
                castRayShadow=True,alphaCutout=False,groundOcclusion=.3)
            row['receivesSnow'] = False
        if row.get('mesh') == 'assets/trees/spruce/spruce_dense.gltf' and not row.get('meshLods'):
            row.update(cullCellSize=16,meshLods=[
                {'mesh':'assets/trees/spruce/woodland_spruce_mid.gltf','distance':65},
                {'mesh':'assets/trees/spruce/woodland_spruce_far.gltf','distance':170}])
    names = {row['name'] for row in result['layers']}
    defaults = {row['name']: row for row in dressing(cold)}
    for row in result['layers']:
        if row['name'] in defaults:
            row.setdefault('avoidTracks', defaults[row['name']]['avoidTracks'])
            row['receivesSnow'] = defaults[row['name']]['receivesSnow']
        if row['name'] == 'woodland_fern_patches':
            row['meshLods'] = lods('woodland_fern',(38,75))
        if row['name'] == 'small_stone_beds':
            row['meshLods'] = lods('pebble_bed',(35,65))
        if row['name'] in defaults and row.get('type') == 'rock':
            row['foliageSssStrength'] = 0
        if row['name'] in ('fractured_rock_slabs','fractured_rock_crags'):
            row['collision'] = 'convex-or-sphere'
        if row.get('mesh','').startswith('assets/terrain_dressing/granite') or \
                row.get('mesh') == 'assets/terrain_dressing/fractured_granite.gltf':
            row.setdefault('avoidTracks', True)
    result['layers'].extend(row for row in dressing(cold) if row['name'] not in names)
    if woodland:
        result['layers'].extend(row for row in ground_dressing() if row['name'] not in names)
        # Deliberate off-track foreground pockets guarantee a useful review
        # without filling the service road or covering the shore's open view.
        fixed = layer('lookout_shrub_pockets','hazel_bush','tree',0,(1,1,1),[1,1],240,
            meshLods=lods('hazel_bush',(32,90)),
            fixedPlacements=[[-13,26,23,1.4],[-6,25,115,1.2],[7,23,68,1.5],
                             [12,-11,32,1.1],[32,-14,95,1.3],[-38,27,-25,1.2]])
        if fixed['name'] not in names: result['layers'].append(fixed)
        ferns = layer('lookout_fern_pockets','woodland_fern','grass',0,(1,1,1),[1,1],110,
            meshLods=lods('woodland_fern',(38,75)),windStrength=.035,
            fixedPlacements=[[-7,25,15,.8],[-4,24,110,.65],[-12,25,70,.85],
                             [-37,28,32,.8],[-42,29,95,.9],[-35,30,-15,.7]])
        if ferns['name'] not in names: result['layers'].append(ferns)
        # The three rock rises use different silhouettes, not the same potato
        # mesh scaled repeatedly. Satellite stones keep the authored anchors.
        for row in result['layers']:
            if row['name'] == 'authored_rock_outcrops':
                placements = row.get('fixedPlacements',[])
                slabs = copy.deepcopy(row); slabs.update(name='authored_slab_outcrops',
                    mesh='assets/terrain_dressing/granite_slab.gltf',meshLods=lods('granite_slab',(65,180)))
                crags = copy.deepcopy(row); crags.update(name='authored_crag_outcrops',
                    mesh='assets/terrain_dressing/granite_crag.gltf',meshLods=lods('granite_crag',(65,180)))
                if 'authored_slab_outcrops' not in names:
                    row['fixedPlacements'] = placements[::3]
                    slabs['fixedPlacements'] = placements[1::3]
                    crags['fixedPlacements'] = placements[2::3]
                    result['layers'].extend([slabs,crags])
                break
    return result


def write(path, value):
    path.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')


def main():
    default_path = ROOT/'terrain_scatter.json'
    original = json.loads(default_path.read_text())
    write(default_path,upgrade(original))
    for path in sorted((ASSETS/'scenery').glob('*.json')):
        profile = json.loads(path.read_text())
        cold = 'winter' in path.stem or 'long_dark' in path.stem
        profile['scatter'] = upgrade(profile.get('scatter',original),cold,path.stem=='woodland_swamp')
        write(path,profile)
    saved_path = ASSETS/'settings/scenery_settings.json'
    saved = json.loads(saved_path.read_text())
    for name,profile in saved['profiles'].items():
        if 'scatter' in profile:
            profile['scatter'] = upgrade(profile['scatter'],'winter' in name or 'long_dark' in name,name=='woodland_swamp')
    write(saved_path,saved)
    print('Updated shared manifest, shipped profiles and known saved-profile art.')


if __name__ == '__main__': main()

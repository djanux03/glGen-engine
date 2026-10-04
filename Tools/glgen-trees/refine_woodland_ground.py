"""Apply the redesigned ground cover to the shipped and saved woodland preset.

Only named stock grass is tuned; custom meshes and existing landmarks keep
their authoring. New debris layers append, preserving old layer-index seeds.
"""
import copy
import json
from pathlib import Path

from dress_terrain_profiles import ground_dressing

ROOT = Path(__file__).resolve().parents[2]


def refine_floor(profile):
    materials = profile.get('renderer',{}).get('materials',{})
    meadow = materials.get('0',{})
    # A sparse meadow needs flattened leaves between the 3D bunches. The old
    # smooth moss/rock material made every open patch look like vacant soil.
    # Only replace recognised stock art, preserving custom authored surfaces.
    stock = ('terraingeneratorassets/materials/textures/Mossy Ground_basecolor.jpg',
             'materials/woodland_ground_8k/textures/woodland_moss_basecolor.jpg',
             'grass/meadow_floor_albedo.png')
    if meadow.get('albedo') in stock:
        meadow.update(albedo='grass/meadow_floor_albedo.png',normal='',roughness='',
                      height='',tiling=1.8,reliefDepth=0)
    litter = materials.get('1',{})
    if 'woodland_forest_litter' in litter.get('albedo',''):
        litter['reliefDepth'] = .015


def refine(manifest, woodland, winter=False):
    seasonal_density = .18 if winter else 1
    for row in manifest['layers']:
        if row.get('mesh') == 'assets/grass/woodland_grass.gltf':
            row.update(windStrength=.045,groundOcclusion=.32,foliageSssStrength=1.0,
                       castRayShadow=False,shadowMesh='assets/grass/woodland_grass_shadow.gltf')
            if row['name'] in ('grass_meadow','woodland_grass'):
                row.update(density=1.7*seasonal_density,patchThreshold=.40,patchScale=1.35,
                           heightScale=[.68,1.22],scale=[.85,1.15],
                           biomes={'meadow':1,'forest':.65,'mountain':.32},
                           alignToNormal=.9,leanMaxDeg=7,cullCellSize=3,
                           maxDrawDistance=115,densityFalloffStart=85,
                           tint=[1,1,1],sinkIntoGround=.015)
                row['meshLods'] = [
                    {'mesh':'assets/grass/woodland_grass_mid.gltf','distance':12},
                    {'mesh':'assets/grass/woodland_grass_far.gltf','distance':40}]
            elif row['name'] == 'grass_tuft':
                row.update(density=.16*seasonal_density,scale=[.85,1.15],heightScale=[1.3,1.8],
                           patchThreshold=.46,cullCellSize=3,leanMaxDeg=5,
                           meshLods=[{'mesh':'assets/grass/woodland_grass_mid.gltf','distance':12},
                                     {'mesh':'assets/grass/woodland_grass_far.gltf','distance':40}])
            if woodland and row['name'] == 'woodland_grass':
                row['biomes'] = {'meadow':1,'forest':.55,'mountain':0}
        if row.get('mesh') == 'assets/grass/woodland_sedges.gltf':
            row.update(windStrength=.06,groundOcclusion=.35,foliageSssStrength=1,
                       castRayShadow=False,shadowMesh='assets/grass/woodland_sedges_shadow.gltf')
    source = next((row for row in manifest['layers'] if row['name'] in
                   ('grass_meadow','woodland_grass') and
                   row.get('mesh') == 'assets/grass/woodland_grass.gltf'),None)
    if source:
        # Append to keep all older layer seeds stable. The low sward is a
        # distinct silhouette/height family, with open moss between the sprigs.
        under = copy.deepcopy(source)
        under.pop('fixedPlacements',None)
        under.update(name='grass_low_sward',mesh='assets/grass/woodland_undergrass.gltf',
                     shadowMesh='assets/grass/woodland_undergrass_shadow.gltf',
                     density=.45*seasonal_density,scale=[.85,1.2],heightScale=[.8,1.2],
                     windStrength=.025,groundOcclusion=.28,leanMaxDeg=3,
                     maxDrawDistance=90,densityFalloffStart=70,patchScale=1.8,
                     patchThreshold=.42,
                     meshLods=[{'mesh':'assets/grass/woodland_undergrass_mid.gltf','distance':10},
                               {'mesh':'assets/grass/woodland_undergrass_far.gltf','distance':30}])
        old = next((r for r in manifest['layers'] if r['name'] == under['name']),None)
        if old is None: manifest['layers'].append(under)
        else: old.update(under)
    names = {row['name'] for row in manifest['layers']}
    for row in ground_dressing() if woodland else []:
        if row['name'] not in names:
            manifest['layers'].append(copy.deepcopy(row))
        elif row['name'] == 'broken_verge_sticks':
            old = next(r for r in manifest['layers'] if r['name'] == row['name'])
            old.update(density=row['density'],biomes=row['biomes'])


def main():
    for path in [ROOT/'terrain_scatter.json',ROOT/'assets/scenery/woodland_swamp.json',
                 ROOT/'assets/settings/scenery_settings.json']:
        doc = json.loads(path.read_text())
        if 'profiles' in doc:
            for name,profile in doc['profiles'].items():
                if 'scatter' in profile:
                    refine(profile['scatter'],name == 'woodland_swamp',name == 'bleak_winter')
                if name == 'woodland_swamp':
                    profile.setdefault('renderer',{}).update(aoRadius=.65,aoBias=.004,aoStrength=2.5,
                                                             shadowSoftness=.03)
                if name in ('alpine_flyby','meadows','woodland_swamp'):
                    refine_floor(profile)
        elif 'scatter' in doc:
            doc.setdefault('terrain',{})['grassCastShadows'] = False
            doc.setdefault('renderer',{}).update(aoRadius=.65,aoBias=.004,aoStrength=2.5,
                                                 shadowSoftness=.03)
            refine(doc['scatter'],True)
            refine_floor(doc)
        else:
            refine(doc,False)
        path.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')
    # Every stock grass preset adopts its mesh's sparse casting proxy. Custom
    # authored grass meshes continue to use their existing shadow policy.
    for path in (ROOT/'assets/scenery').glob('*.json'):
        doc = json.loads(path.read_text())
        if 'scatter' not in doc: continue
        refine(doc['scatter'],path.stem == 'woodland_swamp',path.stem == 'bleak_winter')
        if path.stem in ('alpine_flyby','meadows','woodland_swamp'):
            refine_floor(doc)
        path.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')
    for path in [ROOT/'assets/scenery/alpine_flyby.json',ROOT/'assets/scenery/meadows.json',
                 ROOT/'assets/scenery/woodland_swamp.json']:
        doc = json.loads(path.read_text())
        doc.setdefault('renderer',{}).update(temporalAA=True,temporalSharpness=.035)
        path.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')
    path = ROOT/'assets/settings/scenery_settings.json'
    doc = json.loads(path.read_text())
    for name,profile in doc.get('profiles',{}).items():
        if name in ('alpine_flyby','meadows','woodland_swamp'):
            profile.setdefault('renderer',{}).update(temporalAA=True,temporalSharpness=.035)
    path.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')
    path = ROOT/'assets/settings/graphics_settings.json'
    doc = json.loads(path.read_text())
    doc.update(aoRadius=.65,aoBias=.004,aoStrength=2.5,shadowSoftness=.03,
               temporalAA=True,temporalSharpness=.035)
    path.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')


if __name__ == '__main__': main()

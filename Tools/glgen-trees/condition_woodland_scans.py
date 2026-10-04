"""Condition CC0 Poly Haven grass and ground scans for Woodland & Swamp.

Run with --fetch to obtain the original files, then bake deterministic clump
LODs. The downloaded glTF is a library of separate tufts, not a scatter mesh.
Keep complete curved leaves when reducing detail: simplifying arbitrary edges
collapsed narrow blades into the straight triangular needles this replaces.
Only assets are written here; scenery authoring remains in its profile JSON.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import urllib.request

from PIL import Image
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
GRASS = ROOT / 'assets/grass/woodland_scan'
GROUND = ROOT / 'assets/materials/woodland_scans'
HEADERS = {'User-Agent': 'glGen woodland CC0 asset import',
           'Referer': 'https://polyhaven.com/'}
SURFACES = ['forest_leaves_02', 'forest_floor', 'forest_ground_04',
            'rock_face_03', 'brown_mud_leaves_01']


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS),
                                timeout=180) as response:
        return response.read()


def fetch():
    provenance = {'license': 'CC0-1.0', 'sources': []}
    for name in ['grass_medium_01', 'grass_medium_02', *SURFACES]:
        files = json.loads(get('https://api.polyhaven.com/files/' + name))
        info = json.loads(get('https://api.polyhaven.com/info/' + name))
        if name.startswith('grass_medium_'):
            model = files['gltf']['2k']['gltf']
            jobs = [(GRASS/('source/'+name+'_2k.gltf'), model)]
            jobs += [(GRASS/'source'/p, f) for p, f in model['include'].items()]
            jobs += [(GRASS/('source/'+name+'_alpha_2k.png'),
                      files['Alpha']['2k']['png'])]
        else:
            jobs = []
            for channel in ['Diffuse', 'nor_gl', 'Rough', 'Displacement']:
                entry = files[channel]['4k']['jpg']
                jobs.append((GROUND / entry['url'].split('/')[-1], entry))
        rows = []
        for path, entry in jobs:
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                path.write_bytes(get(entry['url']))
            assert hashlib.md5(path.read_bytes()).hexdigest() == entry['md5'], path
            rows.append({'path': path.relative_to(ROOT).as_posix(), **entry})
        provenance['sources'].append({'asset': name,
            'url': 'https://polyhaven.com/a/' + name,
            'authors': info.get('authors', {}), 'files': rows})
        print('Verified', name, flush=True)
    GROUND.mkdir(parents=True, exist_ok=True)
    (GROUND/'provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')


def accessor(doc, blob, index):
    a = doc['accessors'][index]
    v = doc['bufferViews'][a['bufferView']]
    dtype = {5126: '<f4', 5123: '<u2', 5125: '<u4'}[a['componentType']]
    width = {'VEC3': 3, 'VEC2': 2, 'SCALAR': 1}[a['type']]
    assert 'byteStride' not in v
    offset = a.get('byteOffset', 0) + v.get('byteOffset', 0)
    return np.frombuffer(blob, dtype=dtype, count=a['count']*width,
                         offset=offset).reshape(a['count'], width).copy()


def leaf_groups(indices):
    parent = {int(x): int(x) for x in indices.flat}
    def root(x):
        x = int(x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for tri in indices:
        r = root(tri[0])
        for x in tri[1:]:
            parent[root(x)] = r
    groups = {}
    for tri in indices:
        groups.setdefault(root(tri[0]), []).append(tri)
    # Spatially scattered, stable selection avoids deleting an entire side of
    # the tuft at a distance boundary. Do not use Python's randomized hash.
    return sorted(groups.values(), key=lambda g:
                  ((int(g[0][0])*2654435761) & 0xffffffff))


def export(name, pos, normal, uv, indices, material, source, atlas):
    used = np.unique(indices)
    remap = np.full(len(pos), -1, dtype=np.int32)
    remap[used] = np.arange(len(used))
    indices = remap[indices].astype('<u4')
    # Import recentres each mesh from its bounds. Identical unreferenced
    # envelope vertices keep all LOD/shadow roots in precisely the same place.
    bounds = np.array([pos.min(axis=0), pos.max(axis=0)], dtype='<f4')
    p = np.concatenate([pos[used], bounds]).astype('<f4')
    n = np.concatenate([normal[used], [[0, 1, 0]]*2]).astype('<f4')
    tex = np.concatenate([uv[used], [[0, 0]]*2]).astype('<f4')
    assert np.isfinite(p).all() and np.isfinite(n).all()
    assert np.max(np.abs(np.linalg.norm(n, axis=1)-1)) < .001
    doc = {'asset': {'version': '2.0', 'generator': 'glGen woodland scan conditioner'},
           'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'mesh': 0}],
           'buffers': [{'uri': name+'.bin'}], 'bufferViews': [], 'accessors': [],
           'materials': [material], 'images': copy.deepcopy(source['images']),
           'textures': copy.deepcopy(source['textures']),
           'samplers': copy.deepcopy(source.get('samplers', []))}
    doc['images'][1]['uri'] = atlas
    for i in [0, 2]:
        doc['images'][i]['uri'] = 'source/' + doc['images'][i]['uri']
    data = bytearray()
    for arr, kind, component in [(p, 'VEC3', 5126), (n, 'VEC3', 5126),
                                  (tex, 'VEC2', 5126), (indices, 'SCALAR', 5125)]:
        raw = arr.tobytes()
        view = len(doc['bufferViews'])
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(data),
                                   'byteLength': len(raw)})
        data.extend(raw)
        a = {'bufferView': view, 'componentType': component,
             'count': arr.size if kind == 'SCALAR' else len(arr), 'type': kind}
        if view == 0:
            a.update(min=bounds[0].tolist(), max=bounds[1].tolist())
        doc['accessors'].append(a)
    doc['buffers'][0]['byteLength'] = len(data)
    doc['meshes'] = [{'primitives': [{'attributes': {'POSITION': 0,
        'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}]
    (GRASS/(name+'.bin')).write_bytes(data)
    (GRASS/(name+'.gltf')).write_text(json.dumps(doc, indent=2)+'\n')
    return indices.size//3


def bake():
    counts = {}
    # Meadow tufts are about .60m wide/.40m high, low cover .67m/.22m.
    # Widening the small library clumps adds leaf body without increasing
    # instance density; the original uniform scale produced skinny weeds.
    for name, asset, mesh, scale in [('meadow', 'grass_medium_02', 0, (5.0, 2.6, 5.0)),
             ('low_sward', 'grass_medium_01', 6, (3.6, 2.2, 3.6)),
             ('tall', 'grass_medium_01', 4, 1.7),
             ('marsh', 'grass_medium_02', 2, 3.0)]:
        source = json.loads((GRASS/('source/'+asset+'_2k.gltf')).read_text())
        blob = (GRASS/('source/'+asset+'.bin')).read_bytes()
        # The upstream JPEG glTF cannot store its advertised alpha. Combine
        # the published mask with the unchanged scanned diffuse/gradients.
        atlas = asset+'_woodland_grass_scan_rgba.png'
        color = Image.open(GRASS/('source/textures/'+asset+'_diff_2k.jpg')).convert('RGBA')
        alpha = Image.open(GRASS/('source/'+asset+'_alpha_2k.png')).convert('L')
        assert color.size == alpha.size
        color.putalpha(alpha)
        color.save(GRASS/atlas)
        material = copy.deepcopy(source['materials'][0])
        material.update(alphaMode='MASK', alphaCutoff=.45)
        material['pbrMetallicRoughness']['roughnessFactor'] = .9
        primitive = source['meshes'][mesh]['primitives'][0]
        p = accessor(source, blob, primitive['attributes']['POSITION'])*scale
        n = accessor(source, blob, primitive['attributes']['NORMAL'])
        # Flattening the tuft changes its surface slopes as well as its shape.
        # Preserve inverse-transpose normals instead of lighting a broad leaf
        # as though it still belonged to the original skinny clump.
        n = n / np.asarray(scale)
        n = n / np.linalg.norm(n, axis=1, keepdims=True)
        uv = accessor(source, blob, primitive['attributes']['TEXCOORD_0'])
        idx = accessor(source, blob, primitive['indices']).reshape(-1, 3)
        groups = leaf_groups(idx)
        counts[name] = {'near': export(name, p, n, uv, idx, material, source, atlas)}
        for suffix, fraction in [('mid', .24), ('far', .075), ('shadow', .14)]:
            budget = int(len(idx)*fraction)
            selected, count = [], 0
            for group in groups:
                if count+len(group) <= budget:
                    selected.extend(group)
                    count += len(group)
            assert selected
            counts[name][suffix] = export(name+'_'+suffix, p, n, uv,
                          np.array(selected), material, source, atlas)
    (GRASS/'conditioning.json').write_text(json.dumps({
        'sources': ['https://polyhaven.com/a/grass_medium_01',
                    'https://polyhaven.com/a/grass_medium_02'],
        'license': 'CC0-1.0', 'triangles': counts,
        'policy': 'Complete leaf subsets; shared bounds; unchanged diffuse and UVs; inverse-transpose normals after scaling.'}, indent=2)+'\n')
    print(json.dumps(counts, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    if args.fetch:
        fetch()
    bake()

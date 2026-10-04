"""Bake original shared shrubs, ferns and fractured stones, with render LODs.

Geometry uses a specified integer hash, never platform-dependent random
distributions. Existing foliage/granite textures retain their provenance.
Run: python Tools/glgen-trees/make_terrain_dressing.py
"""
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

from make_woodland_birch import Mesh, add, sub, mul, cross, norm, unit
from make_woodland_grass import gradient_png

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'assets/terrain_dressing'


def material(name, color, roughness=.8):
    return {'name': name, 'doubleSided': True, 'pbrMetallicRoughness': {
        'baseColorFactor': [*color, 1], 'metallicFactor': 0,
        'roughnessFactor': roughness}}


def save(name, parts, materials, images=()):
    # Reject broken assets before they enter the renderer/BLAS builder. These
    # also catch collapsed fracture faces and zero-area pinnae at cheap LODs.
    for m in parts:
        assert len(m.p) == len(m.n) == len(m.uv), name
        assert len(m.idx) % 3 == 0, name
        assert all(math.isfinite(x) for values in (m.p,m.n,m.uv) for v in values for x in v), name
        assert all(.99 < sum(x*x for x in n) < 1.01 for n in m.n), name
        for i in range(0,len(m.idx),3):
            a,b,c = (m.p[v] for v in m.idx[i:i+3])
            area = cross(sub(b,a),sub(c,a))
            assert sum(x*x for x in area) > 1e-14, (name,i)
    for uri in images:
        if not uri.startswith('data:'): assert (OUT/uri).is_file(), (name,uri)
    blob, views, accessors, primitives = bytearray(), [], [], []
    def put(values, fmt, kind, target):
        while len(blob) % 4: blob.append(0)
        data = b''.join(struct.pack('<'+fmt, *(v if isinstance(v, tuple) else (v,)))
                        for v in values)
        views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(data), 'target': target})
        blob.extend(data)
        a = {'bufferView': len(views)-1, 'componentType': 5125 if fmt == 'I' else 5126,
             'count': len(values), 'type': kind}
        if kind == 'VEC3':
            a.update(min=[min(v[i] for v in values) for i in range(3)],
                     max=[max(v[i] for v in values) for i in range(3)])
        accessors.append(a)
        return len(accessors)-1
    for i, m in enumerate(parts):
        if not m.idx: continue
        primitives.append({'attributes': {'POSITION': put(m.p, '3f', 'VEC3', 34962),
            'NORMAL': put(m.n, '3f', 'VEC3', 34962), 'TEXCOORD_0': put(m.uv, '2f', 'VEC2', 34962)},
            'indices': put(m.idx, 'I', 'SCALAR', 34963), 'material': i})
    doc = {'asset': {'version': '2.0', 'generator': 'glGen deterministic terrain dressing'},
        'scene': 0, 'scenes': [{'nodes': [0]}], 'nodes': [{'name': name, 'mesh': 0}],
        'meshes': [{'primitives': primitives}], 'materials': materials,
        'buffers': [{'byteLength': len(blob), 'uri': 'data:application/octet-stream;base64,'+
                     base64.b64encode(blob).decode()}], 'bufferViews': views, 'accessors': accessors}
    if images:
        doc.update(images=[{'uri': uri} for uri in images], textures=[{'source': i} for i in range(len(images))])
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / (name+'.gltf')
    path.write_text(json.dumps(doc, separators=(',', ':'))+'\n', encoding='utf-8')
    return {'triangles': sum(len(m.idx)//3 for m in parts),
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def shrub(seed, low, lod):
    stems, leaves = Mesh(), Mesh()
    # Several leaders from one root, rather than a scaled-down tree crown.
    # Two asymmetric species share their material atlas but have distinct silhouettes.
    count = 7 if low else 5
    for i in range(count):
        angle = i*2.399963 + unit(seed+i)*.6
        height = (.5 if low else 1.05) * (.6+.75*unit(seed+i+11))
        reach = (.8 if low else .6) * (.7+.45*unit(seed+i+21))
        root = (.12*math.cos(angle), -.04, .12*math.sin(angle))
        elbow = (reach*.5*math.cos(angle), height*.55, reach*.5*math.sin(angle))
        tip = (reach*math.cos(angle), height, reach*math.sin(angle))
        if lod < 2:
            stems.stem(root, elbow, .018, .009, 5 if lod == 0 else 3)
            stems.stem(elbow, tip, .009, .0025, 4 if lod == 0 else 3)
        for j in range((14, 5, 3)[lod]):
            k = seed+100+i*79+j*13
            t = .24+j*.72/max(1,(14,5,3)[lod]-1)
            centre = (add(root,mul(sub(elbow,root),t/.55)) if t < .55 else
                      add(elbow,mul(sub(tip,elbow),(t-.55)/.45)))
            az = angle + (j%2*2-1)*(.8+unit(k))
            along = norm((math.cos(az), -.65+1.15*unit(k+1), math.sin(az)))
            side = norm(cross(along, (0, 1, 0)))
            centre = add(centre, mul(along, .1+.22*unit(k+2)))
            size = (.36+.14*unit(k+3)) * (1, 1.25, 1.6)[lod]
            leaves.spray(centre, along, side, size, lod == 0)
    wood = material('hazel woody stems', (.14, .095, .045), .9)
    leaf = material('understory leaf sprays', (.72, .84, .58) if low else (.84, .92, .72), .64)
    leaf.update(alphaMode='MASK', alphaCutoff=.45)
    leaf['pbrMetallicRoughness']['baseColorTexture'] = {'index': 0}
    return save(('low_scrub' if low else 'hazel_bush')+('', '_mid', '_far')[lod],
                [stems, leaves], [wood, leaf], ['../trees/birch/birch_spray_v2.png'])


def fern(lod):
    parts = [Mesh(), Mesh()]
    # Curved fronds with paired, tapered pinnae and a raised centre vein.
    # Silhouette is geometry, so small fern shadows do not need dense alpha cards.
    for i in range(0, 9, (1,2,4)[lod]):
        az = i*2.399963+unit(i+500)*.4
        direction = (math.cos(az), 0, math.sin(az))
        side = (-math.sin(az), 0, math.cos(az))
        length = .7+.35*unit(i+700)
        def centre(t):
            return add(mul(direction, length*.9*t),
                       (0, .05+length*.62*math.sin(t*math.pi*.88), 0))
        for segment in range((5,3,0)[lod]):
            segments = 5 if lod == 0 else 3
            t = segment/segments
            parts[i%2].stem(centre(t), centre((segment+1)/segments),
                            .007*(1-t)+.002, .007*(1-(segment+1)/segments)+.002, 3)
        pairs = (9,5,3)[lod]
        for j in range(pairs):
            t = .14+j*(.8/pairs)
            base = centre(t)
            width = length*.29*math.sin(t*math.pi)**.7
            for sign in (-1, 1):
                tip = add(add(base, mul(side, sign*width)), mul(direction, .12))
                half = mul(direction, .025*(1-t)+.006)
                ridge = add(add(base, mul(sub(tip, base), .48)), (0, .015, 0))
                m = parts[(i+j)%2]
                a, b = sub(base, half), add(base, half)
                m.triangle(a, tip, ridge, uv=((0,0),(.5,1),(.5,.5)))
                m.triangle(tip, b, ridge, uv=((.5,1),(1,0),(.5,.5)))
                m.triangle(b, a, ridge, uv=((1,0),(0,0),(.5,.5)))
    materials = [material('fern green', (1,1,1), .76), material('fern sage', (1,1,1), .8)]
    for i, m in enumerate(materials): m['pbrMetallicRoughness']['baseColorTexture'] = {'index': i}
    images = ['data:image/png;base64,'+base64.b64encode(gradient_png(c)).decode()
              for c in ((.085,.16,.032), (.12,.195,.049))]
    return save('woodland_fern'+('', '_mid', '_far')[lod], parts, materials, images)


def rock_mesh(seed, lod, scale=(1.35,.88,1.05), offset=(0,0,0)):
    points = [(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)]
    faces = [(2,4,0),(2,1,4),(2,5,1),(2,0,5),(3,0,4),(3,4,1),(3,1,5),(3,5,0)]
    for _ in range((3,2,1,0)[lod]):
        cache, next_faces = {}, []
        def midpoint(a,b):
            key = tuple(sorted((a,b)))
            if key not in cache:
                cache[key] = len(points); points.append(norm(add(points[a],points[b])))
            return cache[key]
        for a,b,c in faces:
            ab,bc,ca = midpoint(a,b),midpoint(b,c),midpoint(c,a)
            next_faces.extend([(a,ab,ca),(ab,b,bc),(ca,bc,c),(ab,bc,ca)])
        faces = next_faces
    # Intersect radial samples with a shared set of fracture planes. Unlike
    # per-vertex noise, these create coherent broken shoulders and broad faces.
    planes = [(norm((math.cos(i*2.399+seed), .45*math.sin(i*1.7+seed),
                     math.sin(i*2.399+seed))), .65+.2*unit(seed+i*11)) for i in range(7)]
    planes += [((0,1,0), .75), ((0,-1,0), .82)]
    shaped = []
    for p in points:
        radius = 1+.035*math.sin(p[0]*13+seed)*math.cos(p[2]*11-seed)
        for n,d in planes:
            dot = sum(a*b for a,b in zip(n,p))
            if dot > 0: radius = min(radius,d/dot)
        q = tuple(p[i]*radius*scale[i] for i in range(3))
        shaped.append(add(q, add(offset,(0,scale[1]*.80,0))))
    normals = []
    neighbours = [[] for _ in shaped]
    for fi,(a,b,c) in enumerate(faces):
        n = norm(cross(sub(shaped[b],shaped[a]),sub(shaped[c],shaped[a])))
        normals.append(n)
        for v in (a,b,c): neighbours[v].append(fi)
    mesh = Mesh()
    for fi, face in enumerate(faces):
        normal = normals[fi]
        # World-scale planar UVs avoid stretched granite at sphere poles.
        axis = max(range(3),key=lambda i:abs(normal[i]))
        axes = ((2,1),(0,2),(0,1))[axis]
        uv = [tuple(shaped[v][i]/1.4 for i in axes) for v in face]
        ns = []
        for v in face:
            nearby = [normals[n] for n in neighbours[v]
                      if sum(a*b for a,b in zip(normal,normals[n])) > .88]
            ns.append(norm(tuple(sum(n[i] for n in nearby) for i in range(3))))
        mesh.triangle(*(shaped[v] for v in face), uv=uv, normals=ns)
    return mesh


def rocks(name, seed, scale, lod, small=False):
    m = rock_mesh(seed, lod, scale)
    if small:
        m = Mesh()
        for i in range((7,2,1)[lod]):
            angle = i*2.399+seed
            radius = .55*math.sqrt(unit(seed+i+61))
            size = .06+.13*unit(seed+i+91)
            stone = rock_mesh(seed+i*7, 2 if lod < 2 else 3, (size,size*.55,size*.85),
                              (radius*math.cos(angle),0,radius*math.sin(angle)))
            start = len(m.p); m.p.extend(stone.p); m.n.extend(stone.n); m.uv.extend(stone.uv)
            m.idx.extend(start+x for x in stone.idx)
    mat = material('fractured weathered granite', (1,1,1), .9)
    mat['normalTexture'] = {'index': 1, 'scale': .65}
    mat['pbrMetallicRoughness']['baseColorTexture'] = {'index': 0}
    return save(name+('', '_mid', '_far')[lod], [m], [mat],
                ['../materials/m1/textures/stone-47_diffuse.jpg',
                 '../materials/m1/textures/stone-47_normals.jpg'])


def deadwood():
    m = Mesh()
    for i in range(5):
        az = i*2.399; r=.4*unit(i+1300)
        a = (r*math.cos(az),.05,r*math.sin(az))
        b = add(a, (.7*math.cos(az), .09, .7*math.sin(az)))
        m.stem(a,b,.035,.012,5)
        if i%2 == 0: m.stem(add(a,mul(sub(b,a),.6)),add(b,(.15,.09,-.19)),.013,.003,4)
    return save('forest_deadwood', [m], [material('weathered fallen twigs',(.13,.085,.042),.96)])


def ground_litter():
    """Small crooked branches and folded leaf scraps, shared by many instances."""
    bark, cuts = Mesh(), Mesh()
    def stick(a, b, r0, r1):
        start = len(bark.p)
        bark.stem(a,b,r0,r1,7)
        # The shared tree builder projects V from height. Horizontal fallen
        # branches need distance along their axis or bark collapses to stripes.
        length = math.sqrt(sum(x*x for x in sub(b,a)))
        for j in range(start,len(bark.p)):
            u,_ = bark.uv[j]
            distance = math.sqrt(sum(x*x for x in sub(bark.p[j],a)))
            bark.uv[j] = (u,distance/max(length,1e-5)*length/.35)
        axis = norm(sub(b,a))
        side = norm(cross(axis,(0,1,0)))
        up = cross(axis,side)
        for centre,r,flip in ((a,r0,-1),(b,r1,1)):
            for j in range(7):
                def rim(k):
                    t = k*math.tau/7
                    return add(centre,mul(add(mul(side,math.cos(t)),mul(up,math.sin(t))),r))
                points = (centre,rim(j),rim(j+1))
                cuts.triangle(*(points if flip>0 else points[::-1]))
    # Unequal crooked pieces lie on the soil, with pale exposed breaks and
    # fine forks. The former deadwood was five identical straight spokes.
    for a,b,c,r in [((-.36,.022,-.09),(-.06,.028,-.026),(.28,.018,.135),.018),
                    ((-.16,.013,.22),(.03,.017,.16),(.19,.014,.25),.010)]:
        stick(a,b,r,r*.72)
        stick(b,c,r*.72,.006)
        stick(b,add(b,(-.08,.021,.21)),r*.45,.003)
    wood = material('dark fissured twig bark',(.48,.40,.32),.95)
    wood['pbrMetallicRoughness']['baseColorTexture'] = {'index':0}
    # Reuse the photographed conifer bark; branch UVs retain its longitudinal
    # grain. This is a solid surface, never an alpha card or glowing foliage.
    twigs = save('broken_ground_sticks',[bark,cuts],
        [wood,material('splintered weathered wood',(.15,.10,.052),.98)],
        ['../trees/spruce/bark09.png'])
    leaves = [Mesh(),Mesh(),Mesh()]
    for i in range(7):
        k = 9100+i*31
        az = unit(k)*math.tau
        radius = .5*math.sqrt(unit(k+1))
        centre = (radius*math.cos(az),.008,radius*math.sin(az))
        direction = unit(k+2)*math.tau
        along = (math.cos(direction),0,math.sin(direction))
        side = (-along[2],0,along[0])
        length = .035+.035*unit(k+3)
        width = length*(.38+.15*unit(k+4))
        ridge = add(centre,(0,.003+.003*unit(k+5),0))
        perimeter,uvs = [],[]
        for j in range(18):
            t = j*math.tau/18
            x = math.cos(t)*length
            z = math.sin(t)*width*(.78+.20*math.cos(t*8))
            perimeter.append(add(centre,add(mul(along,x),mul(side,z))))
            uvs.append((.5+z/(width*2),.5+x/(length*2)))
        m = leaves[i%3]
        for j in range(18):
            m.triangle(perimeter[j],ridge,perimeter[(j+1)%18],
                       uv=(uvs[j],(.5,.5),uvs[(j+1)%18]))
    # A small vein/mottle atlas supplies local contrast on the low-relief leaf
    # surfaces. Small scraps with curled outlines replace large plain polygons.
    rows = bytearray()
    for y in range(64):
        rows.append(0)
        v = y/63
        for x in range(32):
            u = x/31
            grain = .70+.42*unit(8171+x+y*32)
            vein = math.exp(-((u-.5)/.016)**2)
            vein += .5*math.exp(-(math.sin(v*math.pi*12-abs(u-.5)*8)/.12)**2)
            shade = grain*(1-.23*min(vein,1))*(.85+.15*math.sin(v*math.pi))
            for c in (.21,.112,.045):
                linear = c*shade
                rows.append(round((1.055*linear**(1/2.4)-.055)*255))
            rows.append(255)
    def chunk(tag,data):
        return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
    png = (b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',32,64,8,6,0,0,0))+
           chunk(b'IDAT',zlib.compress(rows,9))+chunk(b'IEND',b''))
    mats = [material('old oak leaf',(.80,.70,.58),.96),
            material('damp leaf',(.38,.36,.30),.98),
            material('faded leaf',(1,.92,.72),.95)]
    for mat in mats: mat['pbrMetallicRoughness']['baseColorTexture'] = {'index':0}
    litter = save('curled_leaf_litter',leaves,
        mats,['data:image/png;base64,'+base64.b64encode(png).decode()])
    return twigs,litter


def bake():
    report = {}
    for lod in range(3):
        for low in (False, True):
            name = ('low_scrub' if low else 'hazel_bush')+('', '_mid', '_far')[lod]
            report[name] = shrub(110 if low else 431, low, lod)
        for name,seed,scale in [('fractured_granite',17,(1.4,.95,1.2)),
                                 ('granite_slab',53,(1.6,.48,.95)),
                                 ('granite_crag',89,(.95,1.35,.85))]:
            report[name+('', '_mid', '_far')[lod]] = rocks(name,seed,scale,lod)
    for lod in range(2):
        report['woodland_fern'+('' if lod == 0 else '_mid')] = fern(lod)
        report['pebble_bed'+('' if lod == 0 else '_mid')] = rocks('pebble_bed',151,(1,1,1),lod,True)
    report['woodland_fern_far'] = fern(2)
    report['pebble_bed_far'] = rocks('pebble_bed',151,(1,1,1),2,True)
    report['forest_deadwood'] = deadwood()
    report['broken_ground_sticks'],report['curled_leaf_litter'] = ground_litter()
    (OUT/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__': bake()

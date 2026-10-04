"""Bake the woodland preset's birch with photographed-style foliage and bark.

Run from any directory. Writes a self-contained glTF into assets/trees/birch.
A specified integer hash keeps the geometry repeatable without distributions.
UVs follow trunk circumference and height, rather than restarting on every
triangle. Small curved twig sprays give many detailed leaves per triangle;
alpha-tested depth and ray micromaps preserve the gaps and dappled shadows.
"""
import base64
import json
import math
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]


def unit(i):
    x = (i * 0x9E3779B9 + 271828) & 0xffffffff
    x = ((x ^ (x >> 16)) * 0x7FEB352D) & 0xffffffff
    x = ((x ^ (x >> 15)) * 0x846CA68B) & 0xffffffff
    return ((x ^ (x >> 16)) & 0xffffff) / 0xffffff


def add(a, b): return tuple(x + y for x, y in zip(a, b))
def mul(a, s): return tuple(x * s for x in a)
def sub(a, b): return add(a, mul(b, -1))
def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def norm(v): return mul(v, 1 / max(math.sqrt(sum(x*x for x in v)), 1e-8))


class Mesh:
    def __init__(self):
        self.p, self.n, self.uv, self.idx = [], [], [], []

    def triangle(self, a, b, c, normal=None, uv=None, normals=None):
        n = normal or norm(cross(sub(b, a), sub(c, a)))
        offset = len(self.p)
        self.p.extend((a, b, c)); self.n.extend(normals or (n, n, n))
        self.uv.extend(uv or ((0, 0), (1, 0), (.5, 1)))
        self.idx.extend((offset, offset + 1, offset + 2))

    def stem(self, start, end, r0, r1, sides=6):
        axis = norm(sub(end, start))
        side = norm(cross(axis, (0, 0, 1)))
        up = cross(axis, side)
        for k in range(sides):
            a, b = k * math.tau / sides, (k + 1) * math.tau / sides
            na = add(mul(side, math.cos(a)), mul(up, math.sin(a)))
            nb = add(mul(side, math.cos(b)), mul(up, math.sin(b)))
            p, q = add(start, mul(na, r0)), add(start, mul(nb, r0))
            r, s = add(end, mul(nb, r1)), add(end, mul(na, r1))
            # One bark tile spans the circumference and two metres of height.
            # Shared radial normals keep an eight-sided distant stem smooth.
            u, v = k/sides, (k+1)/sides
            y0, y1 = start[1]*.5, end[1]*.5
            self.triangle(p, q, r, uv=((u,y0),(v,y0),(v,y1)), normals=(na,nb,nb))
            self.triangle(p, r, s, uv=((u,y0),(v,y1),(u,y1)), normals=(na,nb,na))

    def spray(self, c, along, side, size, curved):
        n = norm(cross(side, along))
        # A shallow fold reads in silhouette, without dividing every individual
        # leaf into geometry. glTF V=0 is the top of the photographed spray.
        left, right = sub(c,mul(side,size*.5)), add(c,mul(side,size*.5))
        points = [sub(left,mul(along,size*.5)), sub(right,mul(along,size*.5)),
                  add(right,mul(along,size*.5)), add(left,mul(along,size*.5))]
        if curved:
            ridge = add(c,mul(n,size*.09))
            uv = [(0,1),(1,1),(1,0),(0,0)]
            for i in range(4):
                j = (i+1)%4
                self.triangle(points[i],points[j],ridge, n, (uv[i],uv[j],(.5,.5)))
        else:
            self.triangle(*points[:3],n,((0,1),(1,1),(1,0)))
            self.triangle(points[0],points[2],points[3],n,((0,1),(1,0),(0,0)))


def build(lod=0):
    bark, branches, leaves = Mesh(), Mesh(), Mesh()
    # A swept stem with a second leader creates a light, irregular crown.
    def centre(y): return (.025*y + .009*y*y, y, .07*math.sin(y*.6))
    sections = (16,8,4)[lod]
    step = 8 / sections
    for i in range(sections):
        y = i * step
        bark.stem(centre(y), centre(y+step), .16*(1-y/9)+.018,
                  .16*(1-(y+step)/9)+.018, (12,8,4)[lod])
    bark.stem(centre(3.6), (-.65, 7.4, .5), .09, .012, (7,5,4)[lod])
    for i in range(0,38,(1,2,3)[lod]):
        y = 2.2 + i * .14 + (unit(i+720)-.5)*.35
        angle = i * 2.399963 + unit(i+900) * .45
        radius = (1.0 - abs((y-5.3)/3.6)) * (2.2 + unit(i+1000)*.65)
        start = centre(y)
        end = add(start, (math.cos(angle)*radius, .45 + unit(i+1100)*.6,
                          math.sin(angle)*radius))
        if lod<2: branches.stem(start,end,.045*(1-y/9),.006, (5,3)[lod])
        for twig in range((3,2,2)[lod]):
            t = .38 + twig * .25
            base = add(start,mul(sub(end,start),t))
            az = angle + (-1 if twig % 2 else 1) * .7
            tip = add(base,(math.cos(az)*.7,.24,math.sin(az)*.7))
            if lod==0: branches.stem(base,tip,.012,.003,4)
            for leaf in range((3,2,1)[lod]):
                key = 2000 + i*401 + twig*37 + leaf*3
                c = add(base,mul(sub(tip,base),.2 + .9*unit(key)))
                c = add(c,((unit(key+1)-.5)*.75,(unit(key+2)-.5)*.5,
                            (unit(key+3)-.5)*.75))
                azimuth, tilt = unit(key+4)*math.tau, (unit(key+5)-.5)*1.6
                along = (math.cos(azimuth)*math.cos(tilt), math.sin(tilt),
                         math.sin(azimuth)*math.cos(tilt))
                side = norm(cross(along,(0,1,0)))
                size = (.72 + unit(key+6)*.32) * (1,1.6,2.35)[lod]
                leaves.spray(c,along,side,size,lod==0)
    return [bark,branches,leaves]


def bake(lod=0):
    meshes = build(lod)
    blob, views, accessors, primitives = bytearray(), [], [], []
    def push(values, fmt, typ, target):
        while len(blob)%4: blob.append(0)
        data = b''.join(struct.pack('<'+fmt,*(v if isinstance(v,tuple) else (v,))) for v in values)
        views.append({'buffer':0,'byteOffset':len(blob),'byteLength':len(data),'target':target})
        blob.extend(data)
        acc={'bufferView':len(views)-1,'componentType':5125 if fmt=='I' else 5126,
             'count':len(values),'type':typ}
        if target==34962 and typ=='VEC3':
            acc.update(min=[min(v[i] for v in values) for i in range(3)],
                       max=[max(v[i] for v in values) for i in range(3)])
        accessors.append(acc)
        return len(accessors)-1
    for i,m in enumerate(meshes):
        if not m.p: continue
        primitives.append({'attributes':{'POSITION':push(m.p,'3f','VEC3',34962),
                                          'NORMAL':push(m.n,'3f','VEC3',34962),
                                          'TEXCOORD_0':push(m.uv,'2f','VEC2',34962)},
                           'indices':push(m.idx,'I','SCALAR',34963),'material':i})
    materials=[
      {'name':'Papery birch bark','doubleSided':True,'normalTexture':{'index':1},
       'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicFactor':0,'roughnessFactor':.85}},
      {'name':'Fine brown branches','doubleSided':True,'pbrMetallicRoughness':{
       'baseColorFactor':[.16,.13,.085,1],'metallicFactor':0,'roughnessFactor':.88}},
      {'name':'Birch leaf sprays','doubleSided':True,'alphaMode':'MASK','alphaCutoff':.45,
       'pbrMetallicRoughness':{'baseColorTexture':{'index':2},'metallicFactor':0,'roughnessFactor':.52}}]
    doc={'asset':{'version':'2.0','generator':'make_woodland_birch.py'},'scene':0,
         'scenes':[{'nodes':[0]}],'nodes':[{'name':'WoodlandBirch','mesh':0}],
         'meshes':[{'primitives':primitives}],
         'materials':materials,
         'images':[{'uri':name} for name in ('birch_bark_v2.png','birch_bark_normal_v2.png','birch_spray_v2.png')],
         'textures':[{'source':i} for i in range(3)],
         'buffers':[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(blob).decode(),
                     'byteLength':len(blob)}],'bufferViews':views,'accessors':accessors}
    suffix = ('','_mid','_far')[lod]
    out=ROOT/f'assets/trees/birch/woodland_birch{suffix}.gltf'
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(doc,separators=(',',':'))+'\n')
    print(f'{out}: {sum(len(m.idx)//3 for m in meshes)} triangles')


if __name__=='__main__':
    for lod in range(3): bake(lod)

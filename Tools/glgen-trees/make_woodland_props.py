"""Bake original, shared low-cost woodland landmarks. No game assets required.

Fixed placement is in woodland_swamp.json; this script only authors local meshes.
Run: python Tools/glgen-trees/make_woodland_props.py
"""
import base64
import json
import math
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'assets/woodland_props'


def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def add(a,b): return tuple(x+y for x,y in zip(a,b))
def mul(a,s): return tuple(x*s for x in a)
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def unit(a): return mul(a,1/math.sqrt(sum(x*x for x in a)))


class Mesh:
    def __init__(self,materials):
        self.materials=materials
        self.parts=[dict(p=[],n=[],uv=[],i=[]) for _ in materials]

    def quad(self,points,uv,mat=0):
        p=self.parts[mat]; start=len(p['p'])
        n=unit(cross(sub(points[1],points[0]),sub(points[2],points[0])))
        p['p'].extend(points);p['n'].extend([n]*4);p['uv'].extend(uv)
        p['i'].extend(start+i for i in (0,1,2,0,2,3))

    def beam(self,a,b,width,mat=0):
        d=unit(sub(b,a));u=unit(cross(d,(0,1,0) if abs(d[1])<.95 else (1,0,0)))
        v=cross(d,u);u=mul(u,width/2);v=mul(v,width/2)
        corners=[add(mul(u,x),mul(v,y)) for x,y in ((-1,-1),(1,-1),(1,1),(-1,1))]
        for k in range(4):
            j=(k+1)%4
            self.quad([add(a,corners[k]),add(a,corners[j]),add(b,corners[j]),add(b,corners[k])],[(0,0),(1,0),(1,1),(0,1)],mat)

    def trunk(self,a,b,r0,r1,mat=0,sides=12):
        d=unit(sub(b,a));u=unit(cross(d,(0,1,0) if abs(d[1])<.95 else (1,0,0)));v=cross(d,u)
        length=math.sqrt(sum(x*x for x in sub(b,a)))
        for k in range(sides):
            def ring(t,r,c):return add(c,mul(add(mul(u,math.cos(t)),mul(v,math.sin(t))),r))
            t0=k/sides*math.tau;t1=(k+1)/sides*math.tau
            self.quad([ring(t0,r0,a),ring(t1,r0,a),ring(t1,r1,b),ring(t0,r1,b)],
                      [(k/sides,0),((k+1)/sides,0),((k+1)/sides,length/2),(k/sides,length/2)],mat)
        # End grain uses a distinct brown rough material, avoiding bark across cuts.
        for c,r,flip in ((a,r0,-1),(b,r1,1)):
            for k in range(0,sides,2):
                pts=[c]+[add(c,mul(add(mul(u,math.cos(j/sides*math.tau)),mul(v,math.sin(j/sides*math.tau))),r)) for j in (k,k+1,k+2)]
                if flip<0:pts.reverse()
                self.quad(pts,[(.5,.5),(0,0),(1,0),(1,1)],1)

    def save(self,name,images=None):
        blob=bytearray();views=[];accessors=[];primitives=[]
        def put(values,fmt,typ,target):
            while len(blob)%4:blob.append(0)
            data=b''.join(struct.pack('<'+fmt,*(v if isinstance(v,tuple) else (v,))) for v in values)
            views.append(dict(buffer=0,byteOffset=len(blob),byteLength=len(data),target=target));blob.extend(data)
            a=dict(bufferView=len(views)-1,componentType=5125 if fmt=='I' else 5126,count=len(values),type=typ)
            if typ=='VEC3':a.update(min=[min(v[j] for v in values) for j in range(3)],max=[max(v[j] for v in values) for j in range(3)])
            accessors.append(a);return len(accessors)-1
        for mat,p in enumerate(self.parts):
            if not p['i']:continue
            primitives.append(dict(attributes=dict(POSITION=put(p['p'],'3f','VEC3',34962),NORMAL=put(p['n'],'3f','VEC3',34962),TEXCOORD_0=put(p['uv'],'2f','VEC2',34962)),indices=put(p['i'],'I','SCALAR',34963),material=mat))
        doc=dict(asset=dict(version='2.0',generator='glGen original woodland landmarks'),buffers=[dict(byteLength=len(blob),uri='data:application/octet-stream;base64,'+base64.b64encode(blob).decode())],bufferViews=views,accessors=accessors,materials=self.materials,meshes=[dict(primitives=primitives)],nodes=[dict(mesh=0)],scenes=[dict(nodes=[0])],scene=0)
        if images:doc.update(images=[dict(uri=i) for i in images],textures=[dict(source=i) for i in range(len(images))])
        OUT.mkdir(exist_ok=True);(OUT/(name+'.gltf')).write_text(json.dumps(doc,separators=(',',':'))+'\n')
        print(name,sum(len(p['i'])//3 for p in self.parts),'triangles')


def material(name,color,rough=.8,metal=0):
    return dict(name=name,doubleSided=True,pbrMetallicRoughness=dict(baseColorFactor=[*color,1],roughnessFactor=rough,metallicFactor=metal))


def boulder(seed):
    rock=material('weathered granite',(1,1,1),.92)
    rock['pbrMetallicRoughness']['baseColorTexture']=dict(index=0)
    rock['normalTexture']=dict(index=1,scale=.7)
    mesh=Mesh([rock]);p=mesh.parts[0]
    rings,sides=14,24
    for j in range(rings+1):
        t=(j+.001)/(rings+.002)*math.pi
        for i in range(sides+1):
            a=i/sides*math.tau
            # Smooth, low-frequency lobes and a slanted broken shoulder avoid
            # the old untextured geosphere silhouette; fine relief is normal-mapped.
            r=1+.13*math.sin(3*a+seed)*math.sin(t)**2+.08*math.cos(5*a-2*t+seed)
            x=1.6*r*math.sin(t)*math.cos(a)
            z=1.25*r*math.sin(t)*math.sin(a)
            y=.95+1.05*math.cos(t)+.16*x+.12*math.sin(a*2+seed)*math.sin(t)
            # A weathered fracture plane breaks the rounded summit.
            y=min(y,1.7+.10*x-.08*z)
            p['p'].append((x,y,z));p['n'].append((0,0,0))
            p['uv'].append((i/sides*2,j/rings))
    for j in range(rings):
        for i in range(sides):
            k=j*(sides+1)+i
            p['i'].extend((k,k+1,k+sides+1,k+1,k+sides+2,k+sides+1))
    normals=[[0,0,0] for _ in p['p']]
    for a,b,c in zip(p['i'][::3],p['i'][1::3],p['i'][2::3]):
        n=cross(sub(p['p'][b],p['p'][a]),sub(p['p'][c],p['p'][a]))
        for i in (a,b,c):
            normals[i]=[x+y for x,y in zip(normals[i],n)]
    for j in range(rings+1):
        a=j*(sides+1);b=a+sides
        n=add(normals[a],normals[b]);normals[a]=normals[b]=n
    p['n']=[unit(n) for n in normals]
    mesh.save('granite_boulder', ['../materials/m1/textures/stone-47_diffuse.jpg',
                                '../materials/m1/textures/stone-47_normals.jpg'])


def bake():
    boulder(7)
    metal=material('weathered galvanized steel',(.23,.25,.23),.72,.65)
    ceramic=material('dark ceramic insulators',(.065,.07,.055),.34)
    tower=Mesh([metal,ceramic])
    levels=[(0,2.8),(4,2.25),(8,1.7),(12,1.15),(16,.65),(21,.24)]
    for (y0,r0),(y1,r1) in zip(levels,levels[1:]):
        for a,b in (((-1,-1),(1,-1)),((1,-1),(1,1)),((1,1),(-1,1)),((-1,1),(-1,-1))):
            p0=(a[0]*r0,y0,a[1]*r0);p1=(a[0]*r1,y1,a[1]*r1)
            q0=(b[0]*r0,y0,b[1]*r0);q1=(b[0]*r1,y1,b[1]*r1)
            tower.beam(p0,p1,.14);tower.beam(p0,q0,.10)
            tower.beam(p0,q1,.085);tower.beam(q0,p1,.085)
    for y,width in ((17,4.4),(20,3.6)):
        tower.beam((-width,y,0),(width,y,0),.16)
        for sign in (-1,1):
            tower.beam((0,y+1.2,0),(width*sign,y,0),.1)
            tower.beam((width*sign,y,0),(width*sign,y-.8,0),.16,1)
    tower.save('service_pylon')
    cables=Mesh([material('power cables',(.035,.039,.034),.86)])
    for y,z in ((16.2,-4.4),(16.2,4.4),(19.2,-3.6),(19.2,3.6)):
        # span axis follows world X; branches of the tower arms follow local X,
        # so pylons are placed rotated 90 degrees in the profile.
        for i in range(32):
            t0=i/32;t1=(i+1)/32
            cables.beam((160*t0,y-3.5*4*t0*(1-t0),z),(160*t1,y-3.5*4*t1*(1-t1),z),.045)
    cables.save('service_cables')
    bark=material('fallen birch bark',(1,1,1),.86)
    bark['pbrMetallicRoughness']['baseColorTexture']=dict(index=0)
    bark['normalTexture']=dict(index=1,scale=.75)
    log=Mesh([bark,material('cut wood',(.29,.18,.085),.92)])
    log.trunk((-2.9,.38,0),(2.7,.3,.18),.36,.24)
    for a,b,r in (((-.9,.38,.04),(-1.6,.55,1.05),.09),((.8,.33,.12),(1.7,.48,-.8),.065),((1.9,.31,.15),(2.5,.7,.72),.04)):
        log.trunk(a,b,r,.018,sides=6)
    log.save('fallen_birch',['../trees/birch/birch_bark_v2.png','../trees/birch/birch_bark_normal_v2.png'])
    sleepers=Mesh([material('old timber',(.12,.087,.055),.95)])
    for i in range(5):
        sleepers.beam((-1.2,.13+i*.12,-.5+i*.19),(1.4-i*.1,.13+i*.12,-.3+i*.19),.19)
    sleepers.save('discarded_timber')


if __name__=='__main__':bake()

"""Bake softly folded grass blades, distant cutouts and sparse blade shadows.

Small botanical bunches grow throughout an irregular footprint. Near blades use
varied root-to-tip colour atlases and a shallow fold; distant cutouts carry fine
leaf silhouettes. Short sprigs and shadow proxies use actual blades. All placement and shape
choices use an integer hash so baking is reproducible.
Run: python Tools/glgen-trees/make_woodland_grass.py
"""
import base64
import json
import math
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]


def unit(i):
    x = (i * 0x9E3779B9 + 61331) & 0xffffffff
    x = ((x ^ (x >> 16)) * 0x7FEB352D) & 0xffffffff
    x = ((x ^ (x >> 15)) * 0x846CA68B) & 0xffffffff
    return ((x ^ (x >> 16)) & 0xffffff) / 0xffffff


def norm(v):
    n = math.sqrt(sum(x*x for x in v))
    return tuple(x/n for x in v)


def png_rgba(w,h,rows):
    def chunk(tag, data):
        return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR',struct.pack('>2I5B',w,h,8,6,0,0,0))
            + chunk(b'IDAT',zlib.compress(rows,9)) + chunk(b'IEND',b''))


ATLAS_COLUMNS = 8


def blade_normal_png():
    # Fine lengthwise ribs and a softly rounded cross-section give the ribbons
    # surface detail without tessellating veins into every scattered blade.
    w,h = 128,128
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            u,t = (x%16)/15,y/(h-1)
            nx = (u-.5)*.22+.035*math.sin(u*math.pi*6)
            ny = .018*math.sin(t*math.pi*3+u*3)
            n = norm((nx,ny,1))
            rows.extend(round((c*.5+.5)*255) for c in n)
            rows.append(255)
    return png_rgba(w,h,rows)


def gradient_png(colours, seed):
    # Each column is a complete leaf. Different hues along its length matter:
    # multiplying one green by brightness made dark panels with identical tips.
    # Embedded glTF pixels upload without a vertical flip: row zero is UV V=0.
    w, h = 128, 128
    rows = bytearray()
    for y in range(h):
        t = y/(h-1)
        rows.append(0)
        for x in range(w):
            column, u = x//16, (x%16)/15
            root, body, tip = colours
            body_at = .40+.16*unit(seed+column*31)
            a,b = (root,body) if t<body_at else (body,tip)
            blend = t/body_at if t<body_at else (t-body_at)/(1-body_at)
            blend = blend*blend*(3-2*blend)
            # Broad, soft midrib; no noisy stripes that sharpen into aliasing.
            vein = .94+.06*math.cos((u-.5)*math.pi*2)
            variation = .87+.24*unit(seed+column*17+4)
            warmth = (unit(seed+column*19+8)-.5)*.10
            for channel,(ca,cb) in enumerate(zip(a,b)):
                tint = (1+warmth,1,1-warmth)[channel]
                linear = min(1, (ca+(cb-ca)*blend)*variation*vein*tint)
                srgb = 12.92*linear if linear <= .0031308 else 1.055*linear**(1/2.4)-.055
                rows.append(round(srgb*255))
            rows.append(255)
    return png_rgba(w,h,rows)


def bake(name, height, spread, blades, seed, stride=1, bands=6, shadow=False, bounds=None,
         width_scale=1.0, lean_scale=1.0, green_fraction=.60, straw_fraction=.15,
         head_every=32, palette=None):
    parts = [{'p':[], 'n':[], 'uv':[], 'idx':[]} for _ in range(3)]
    def quad(m, points, normal, uv=((0,0),(1,0),(1,1),(0,1))):
        start = len(m['p'])
        m['p'].extend(points); m['n'].extend([normal]*4); m['uv'].extend(uv)
        m['idx'].extend(start+j for j in (0,1,2,0,2,3))
    def seed_head(base, key):
        if bands <= 2 or shadow or height < .5: return
        m = parts[1]
        az = unit(key+90)*math.tau
        side = (math.cos(az),0,-math.sin(az))
        n = (math.sin(az),0,math.cos(az))
        h = height*(.91+.19*unit(key+91))
        foot = (base[0],-.012,base[1])
        top = (base[0]+n[0]*.12,h,base[1]+n[2]*.12)
        # Slender flowering stalks and tiny alternating spikelets, not broad
        # wheat cards. A few pale heads break up the meadow's green silhouette.
        for normal,s in ((n,side),(side,n)):
            quad(m,[tuple(p[j]+s[j]*sign*.0018 for j in range(3))
                    for p,sign in ((foot,-1),(foot,1),(top,1),(top,-1))],normal)
        for j in range(8 if stride == 1 else 3):
            t = .82+.17*j/(7 if stride == 1 else 2)
            c = tuple(foot[k]+(top[k]-foot[k])*t for k in range(3))
            sign = -1 if j%2 else 1
            tip = tuple(c[k]+side[k]*sign*.025+(0,.022,0)[k] for k in range(3))
            quad(m,[tuple(p[k]+n[k]*s*.003 for k in range(3))
                    for p,s in ((c,-1),(c,1),(tip,1),(tip,-1))],side)
    for i in range(0, blades, stride):
        key = seed+i*29
        family = 0 if unit(key+1)<green_fraction else (1 if unit(key+1)<green_fraction+straw_fraction else 2)
        m = parts[family]
        # Small botanical bunches grow throughout the footprint. An even bed
        # of individual roots looked like isolated wires. Unlike the old stars,
        # these bunches have offset roots, unequal height and a shared lean.
        bunch = i//8
        root_key = seed+bunch*173
        radius = spread*math.sqrt((bunch+.25+.5*unit(root_key+2))/math.ceil(blades/8))
        azimuth = bunch*2.39996323+unit(seed)*math.tau+(unit(root_key+3)-.5)*.65
        root_radius = (.035+.065*unit(root_key+14))*math.sqrt(unit(key+15))
        root_angle = unit(key+16)*math.tau
        base = (radius*math.cos(azimuth)+root_radius*math.cos(root_angle),
                radius*math.sin(azimuth)+root_radius*math.sin(root_angle))
        direction = unit(root_key+4)*math.tau+(unit(key+17)-.5)*2.8
        # Mostly upright leaves with a few low sprigs. Excessive curl formerly
        # turned overlapping patches into a broad, diagonal ribbon lattice.
        # Hash the layer choice: i%3 made EVERY stride-3 LOD blade short.
        low = unit(key+12) < .18
        length = height * ((.23+.28*unit(key+5)) if low else
                           (.40+.48*unit(root_key+18))*(.80+.25*unit(key+5)))
        width = ((.012+.010*unit(key+6)) if low else (.014+.008*unit(key+6)))
        width *= width_scale
        # Shadow proxies retain actual blade widths instead of compensating
        # for missing leaves as the distant raster meshes do.
        if not shadow: width *= math.sqrt(stride)
        lean = (.12 if low else .07) + .17*unit(key+7)
        lean *= lean_scale
        bend = (.45 if low else .30) + .48*unit(key+8)
        # A few short, spent blades curl outward at the base; dry grass should
        # punctuate the sward, not form a bright wire cage above every patch.
        if family == 1:
            length *= .85
            lean += .07
        atlas_column = min(ATLAS_COLUMNS-1,int(unit(key+41)*ATLAS_COLUMNS))
        columns = 3 if stride == 1 else 2
        side = (math.cos(direction), 0, -math.sin(direction))
        offset = len(m['p'])
        # Far geometry retains patch footprints and colour families, but
        # substitutes fewer wider blades once individual tips are sub-pixel.
        for row in range(bands+1):
            t = row/bands
            a = lean + bend*t
            distance = length/bend*(math.cos(lean)-math.cos(a))
            y = length/bend*(math.sin(a)-math.sin(lean))-.012
            c = (base[0]+math.sin(direction)*distance,y,
                 base[1]+math.cos(direction)*distance)
            # A narrow sheath widens just above the root, then tapers to a tip.
            # Continuous taper avoids the broad paddle/abrupt triangular tip.
            taper = (1-t)**.45
            half_width = width*.5*(.60+.50*math.sin(t*math.pi))*taper
            n = norm((math.sin(direction)*math.cos(a), -math.sin(a),
                      math.cos(direction)*math.cos(a)))
            for sign in ((-1,0,1) if columns == 3 else (-1,1)):
                ridge = half_width*.09 if sign == 0 else 0
                m['p'].append(tuple(c[j]+side[j]*half_width*sign+n[j]*ridge for j in range(3)))
                m['n'].append(norm(tuple(n[j]+side[j]*sign*.09 for j in range(3))))
                # Leave a margin within each column to keep mip filtering from
                # sampling the neighbouring leaf's colour at its folded edge.
                m['uv'].append(((atlas_column+.10+.80*(sign+1)/2)/ATLAS_COLUMNS,t))
        for row in range(bands):
            for col in range(columns-1):
                a = offset+row*columns+col
                m['idx'].extend((a,a+1,a+columns))
                if row<bands-1: m['idx'].extend((a+1,a+columns+1,a+columns))
        if head_every and i%head_every == 0:
            seed_head(base,key)

    if bounds is None:
        points = [p for m in parts for p in m['p']]
        bounds = tuple(tuple((min(f(p[j] for p in points),-.035) if j==1 and sign<0
                              else f(p[j] for p in points))+(sign*.10 if j!=1 else 0)
                             for j in range(3)) for f,sign in ((min,-1),(max,1)))
    # The importer recentres each file from its bounds. Shared envelope
    # vertices include the wider raster LOD leaves, keeping every LOD and
    # the shadow subset registered instead of shifting their roots sideways.
    parts[0]['p'].extend(bounds)
    parts[0]['n'].extend([(0,1,0)]*2)
    parts[0]['uv'].extend([(0,0)]*2)
    colours=[('Olive meadow leaf',((.045,.086,.023),(.135,.190,.054),(.245,.270,.102))),
             ('Spent straw leaf',((.100,.085,.035),(.190,.170,.069),(.300,.270,.139))),
             ('Shaded green leaf',((.035,.062,.020),(.095,.140,.040),(.170,.200,.070)))]
    if palette is not None: colours=palette
    materials=[{'name':label,'doubleSided':True,'normalTexture':{'index':3,'scale':.30},
                'pbrMetallicRoughness':{'baseColorTexture':{'index':i},
                    'metallicFactor':0,'roughnessFactor':.82}}
               for i,(label,_) in enumerate(colours)]
    images=[{'uri':'data:image/png;base64,'+base64.b64encode(gradient_png(c,seed+i*97)).decode()}
            for i,(_,c) in enumerate(colours)]+[{'uri':'data:image/png;base64,'+
                base64.b64encode(blade_normal_png()).decode()}]
    write_parts(name,parts,materials,images)
    return bounds


def write_parts(name,parts,materials,images):
    blob,views,accessors,primitives = bytearray(),[],[],[]
    # Reject bad tips/folds before the glTF parser or renderer sees them.
    # Collapsed final-row vertices are intentional, but no triangle uses two
    # of them: each final band has exactly one triangle per side of the fold.
    for m in parts:
        assert all(math.isfinite(x) for values in (m['p'],m['n'],m['uv']) for v in values for x in v)
        assert all(.99 < sum(x*x for x in n) < 1.01 for n in m['n'])
        for j in range(0,len(m['idx']),3):
            a,b,c = (m['p'][v] for v in m['idx'][j:j+3])
            ab,ac = tuple(b[k]-a[k] for k in range(3)),tuple(c[k]-a[k] for k in range(3))
            area = (ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0])
            assert sum(x*x for x in area) > 1e-14, (name,j)
    def push(values,fmt,typ,target):
        while len(blob)%4: blob.append(0)
        data=b''.join(struct.pack('<'+fmt,*(v if isinstance(v,tuple) else (v,))) for v in values)
        views.append({'buffer':0,'byteOffset':len(blob),'byteLength':len(data),'target':target})
        blob.extend(data)
        acc={'bufferView':len(views)-1,'componentType':5125 if fmt=='I' else 5126,
             'count':len(values),'type':typ}
        if typ=='VEC3':
            acc.update(min=[min(v[j] for v in values) for j in range(3)],
                       max=[max(v[j] for v in values) for j in range(3)])
        accessors.append(acc)
        return len(accessors)-1
    for i,m in enumerate(parts):
        if not m['p']: continue
        primitives.append({'attributes':{'POSITION':push(m['p'],'3f','VEC3',34962),
                                         'NORMAL':push(m['n'],'3f','VEC3',34962),
                                         'TEXCOORD_0':push(m['uv'],'2f','VEC2',34962)},
                           'indices':push(m['idx'],'I','SCALAR',34963),'material':i})
    doc={'asset':{'version':'2.0','generator':'make_woodland_grass.py'},'scene':0,
         'scenes':[{'nodes':[0]}],'nodes':[{'name':name,'mesh':0}],
         'meshes':[{'primitives':primitives}],
         'materials':materials,'images':images,
         'textures':[{'source':i,'sampler':0} for i in range(len(images))],
         'samplers':[{'magFilter':9729,'minFilter':9987,'wrapS':33071,'wrapT':33071}],
         'buffers':[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(blob).decode(),
                     'byteLength':len(blob)}],'bufferViews':views,'accessors':accessors}
    out=ROOT/f'assets/grass/{name}.gltf'
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(doc,separators=(',',':'))+'\n')
    print(f'{out.name}: {sum(len(m["idx"])//3 for m in parts)} triangles')


def bake_cards(name,height,spread,seed,detail=0,bounds=None):
    """Curved cutouts carry photographed leaf shapes, not opaque paddle edges.

    The four shared atlas columns vary naturally along each leaf. Three gently
    folded views per bunch resolve volume nearby; reduced meshes keep the same
    deterministic bunch centres. Shadows remain sparse real-blade geometry.
    """
    m={'p':[],'n':[],'uv':[],'idx':[]}
    bunches=range(9) if detail<2 else range(0,9,3)
    cards,bands,columns=((3,5,3),(1,2,2),(2,1,2))[detail]
    for bunch in bunches:
        root_key=seed+bunch*173
        radius=spread*math.sqrt((bunch+.25+.5*unit(root_key+2))/9)
        azimuth=bunch*2.39996323+unit(seed)*math.tau+(unit(root_key+3)-.5)*.65
        base=(radius*math.cos(azimuth),radius*math.sin(azimuth))
        length=height*(.40+.48*unit(root_key+18))
        for card in range(cards):
            key=root_key+card*37
            direction=unit(root_key+4)*math.tau+card*math.pi/cards
            side=(math.cos(direction),0,-math.sin(direction))
            forward=(math.sin(direction),0,math.cos(direction))
            width=length*(.75+.12*unit(key+5))
            atlas_column=min(3,int(unit(key+41)*4))
            offset=len(m['p'])
            for row in range(bands+1):
                t=row/bands
                # The texture owns the tiny leaf bends; the card itself has a
                # broad, shallow curve and fold for a continuous light response.
                lean=.10*length*t*t
                n=norm((forward[0],-.20*t,forward[2]))
                for sign in ((-1,0,1) if columns==3 else (-1,1)):
                    fold=width*.035 if sign==0 else 0
                    m['p'].append((base[0]+side[0]*width*.5*sign+forward[0]*(lean+fold),
                                   length*t-.030,
                                   base[1]+side[2]*width*.5*sign+forward[2]*(lean+fold)))
                    m['n'].append(norm(tuple(n[j]+side[j]*sign*.07 for j in range(3))))
                    # The renderer uploads decoded glTF images in top-down order.
                    m['uv'].append(((atlas_column+.035+.93*(sign+1)/2)/4,1-t))
            for row in range(bands):
                for col in range(columns-1):
                    a=offset+row*columns+col
                    m['idx'].extend((a,a+1,a+columns,a+1,a+columns+1,a+columns))
    if bounds is None:
        bounds=tuple(tuple(f(p[j] for p in m['p'])+(sign*.08 if j!=1 else 0)
                           for j in range(3)) for f,sign in ((min,-1),(max,1)))
    # All LODs and the sparse proxy must import with one common centre. Include
    # root spread outside the photographed silhouette in that shared envelope.
    m['p'].extend(bounds);m['n'].extend([(0,1,0)]*2);m['uv'].extend([(0,0)]*2)
    write_parts(name,[m],[{'name':'Natural meadow grass cutouts','doubleSided':True,
        'alphaMode':'MASK','alphaCutoff':.5,
        'pbrMetallicRoughness':{'baseColorTexture':{'index':0},
                              'metallicFactor':0,'roughnessFactor':.90}}],
        [{'uri':'meadow_grass_atlas.png'}])
    return bounds


if __name__=='__main__':
    for name,height,spread,blades,seed in [('woodland_grass',.55,.58,72,61331),
                                         ('woodland_sedges',1.18,.50,72,74957)]:
        bounds = bake(name,height,spread,blades,seed)
        bake(name+'_mid',height,spread,blades,seed,stride=2,bands=3,bounds=bounds)
        bake_cards(name+'_far',height,spread,seed,detail=2,bounds=bounds)
        # One real blade per bunch casts direct shadows.
        # Dense root shading comes from SSAO rather than a black line per leaf.
        bake(name+'_shadow',height,spread,blades,seed,stride=8,bands=2,shadow=True,bounds=bounds)
    # Sparse short sprigs let moss and flattened ground litter remain visible.
    bounds = bake('woodland_undergrass',.22,.54,24,98113,bands=4)
    bake('woodland_undergrass_mid',.22,.54,24,98113,stride=3,bands=2,bounds=bounds)
    bake('woodland_undergrass_far',.22,.54,24,98113,stride=6,bands=2,bounds=bounds)
    bake('woodland_undergrass_shadow',.22,.54,24,98113,stride=6,bands=2,shadow=True,bounds=bounds)

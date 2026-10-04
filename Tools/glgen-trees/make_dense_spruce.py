"""Bake a dense Norway-spruce glTF from the photo textures in assets/trees/spruce.

Why this exists: the shipped spruce.gltf is a young, sparse tree -- a few
dozen needle cards on a bare pole. Scattered at forest density it reads as a
stand of dead snags from any distance. A mature spruce is a closed cone of
drooping branch whorls, and the silhouette, the self-shadowing and the
dappled light through it all come from that density.

The mesh is built the way production conifers are: a tapered bark trunk plus
whorls of branches, each branch dressed with crossed alpha-cutout cards that
carry a photographed needle spray. Two details matter more than they look:

* The spray texture's stem runs corner to corner (bottom-left -> top-right),
  so each card is a square laid DIAGONALLY along its branch -- base corner at
  the trunk, opposite corner at the tip -- and the needles fill a diamond
  around the limb instead of a strip with bare ends.
* Card normals are bent toward the crown's outward direction. A lone card's
  face normal flips shading from lit to unlit every few centimetres; a crown
  is lit like a rough cone, which is what the eye expects of a tree.

Deterministic: a fixed seed and Python's own Mersenne Twister (its sequence is
specified across versions), so re-running reproduces the same bytes.

Usage:  python Tools/glgen-trees/make_dense_spruce.py
Writes: assets/trees/spruce/spruce_dense.gltf + spruce_dense.bin
"""

import json
import math
import os
import random
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, '..', '..', 'assets', 'trees', 'spruce'))
SEED = 51791

HEIGHT = 5.0          # model units; the scatter layer scales to 10-16 m
CROWN_BASE = 0.10     # fraction of height where the lowest whorl sits
MAX_RADIUS = 1.05     # crown radius at its widest (~0.42 of height: a spruce)
TRUNK_RADIUS = 0.085
WHORL_SPACING = 0.17


def v_add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def v_sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def v_mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def v_dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def v_cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def v_norm(a):
    l = math.sqrt(v_dot(a, a)) or 1.0
    return (a[0] / l, a[1] / l, a[2] / l)
def v_lerp(a, b, t): return v_add(a, v_mul(v_sub(b, a), t))


class Prim:
    def __init__(self):
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []

    def vert(self, p, n, uv):
        self.pos.append(p); self.nrm.append(v_norm(n)); self.uv.append(uv)
        return len(self.pos) - 1

    def tri(self, a, b, c):
        self.idx += [a, b, c]


def build_trunk(prim, rng):
    segs, radial = 12, 7
    # Slight sweep so the stem is not a CG-perfect rod.
    lean = (rng.uniform(-0.04, 0.04), rng.uniform(-0.04, 0.04))
    rings = []
    for i in range(segs + 1):
        t = i / segs
        y = t * HEIGHT
        r = TRUNK_RADIUS * (1.0 - t) ** 0.85 + 0.004
        # Root flare over the bottom 4%.
        r *= 1.0 + 0.6 * max(0.0, 1.0 - t / 0.04)
        cx, cz = lean[0] * t * t * HEIGHT, lean[1] * t * t * HEIGHT
        ring = []
        for k in range(radial + 1):
            a = 2 * math.pi * k / radial
            n = (math.cos(a), 0.0, math.sin(a))
            p = (cx + n[0] * r, y, cz + n[2] * r)
            # Bark tiles every ~0.8 m up the stem, once around it.
            ring.append(prim.vert(p, n, (k / radial, y / 0.8)))
        rings.append(ring)
    for i in range(segs):
        for k in range(radial):
            a, b = rings[i][k], rings[i][k + 1]
            c, d = rings[i + 1][k + 1], rings[i + 1][k]
            prim.tri(a, c, b); prim.tri(a, d, c)
    return lean


def trunk_centre(lean, y):
    t = y / HEIGHT
    return (lean[0] * t * t * HEIGHT, y, lean[1] * t * t * HEIGHT)


def crown_radius(t):
    # t = 0 at the crown base, 1 at the leader. Slightly convex cone with a
    # narrowed base (lower branches die back under their own shade).
    cone = (1.0 - t) ** 0.92
    base = min(1.0, 0.72 + t * 3.0)
    return MAX_RADIUS * cone * base


def add_spray_card(prim, base, tip, roll_axis_up, roll, crown_out, width_scale):
    """A square card whose DIAGONAL runs base -> tip (see module doc)."""
    along = v_sub(tip, base)
    length = math.sqrt(v_dot(along, along))
    if length < 1e-3:
        return
    d = v_mul(along, 1.0 / length)
    side0 = v_norm(v_cross(d, roll_axis_up))
    up0 = v_cross(side0, d)
    # Roll the card about its own branch axis.
    side = v_add(v_mul(side0, math.cos(roll)), v_mul(up0, math.sin(roll)))
    face = v_norm(v_cross(side, d))
    half = length * 0.5 * width_scale
    mid = v_mul(v_add(base, tip), 0.5)
    left = v_add(mid, v_mul(side, half))
    right = v_sub(mid, v_mul(side, half))
    # Shading normal: mostly the crown's outward/upward direction, a little
    # of the card's own facing so neighbouring cards do not shade identically.
    def shade(p):
        out = v_norm(v_add(crown_out, (0.0, 0.45, 0.0)))
        f = face if v_dot(face, out) >= 0 else v_mul(face, -1.0)
        return v_norm(v_add(v_mul(out, 0.78), v_mul(f, 0.22)))
    # glTF UVs: origin top-left. The sprig's stem starts bottom-left.
    i0 = prim.vert(base, shade(base), (0.0, 1.0))
    i1 = prim.vert(right, shade(right), (1.0, 1.0))
    i2 = prim.vert(tip, shade(tip), (1.0, 0.0))
    i3 = prim.vert(left, shade(left), (0.0, 0.0))
    prim.tri(i0, i1, i2); prim.tri(i0, i2, i3)


def build_crown(prim, rng, lean):
    y0 = CROWN_BASE * HEIGHT
    whorls = int((HEIGHT * 0.97 - y0) / WHORL_SPACING)
    golden = 2.39996
    phase = 0.0
    for w in range(whorls):
        y = y0 + w * WHORL_SPACING + rng.uniform(-0.03, 0.03)
        t = (y - y0) / (HEIGHT - y0)
        radius = crown_radius(t)
        count = max(3, int(round(7.5 - 3.0 * t + rng.uniform(-1.0, 1.0))))
        phase += golden
        centre = trunk_centre(lean, y)
        for b in range(count):
            az = phase + 2 * math.pi * b / count + rng.uniform(-0.25, 0.25)
            out = (math.cos(az), 0.0, math.sin(az))
            length = radius * rng.uniform(0.82, 1.08)
            if length < 0.08:
                continue
            # Upper branches reach up, lower ones sweep down (Picea abies);
            # the tip turns back up a little.
            pitch = math.radians(18.0 * t - 22.0 * (1.0 - t) + rng.uniform(-6, 6))
            d = v_norm((out[0] * math.cos(pitch), math.sin(pitch), out[2] * math.cos(pitch)))
            base = v_add(centre, v_mul(out, TRUNK_RADIUS * (1.0 - t) * 0.8))
            mid = v_add(base, v_mul(d, length * 0.55))
            tip = v_add(v_add(base, v_mul(d, length)), (0.0, length * 0.10, 0.0))
            crown_out = v_norm((out[0], 0.25 + 0.5 * t, out[2]))
            # Two segments (droop then lift), each dressed with a crossed
            # pair of cards: one near horizontal, one rolled ~65 degrees so
            # the branch still reads edge-on from the side.
            for (s0, s1) in ((base, mid), (v_lerp(base, mid, 0.55), tip)):
                roll = rng.uniform(-0.25, 0.25)
                add_spray_card(prim, s0, s1, (0, 1, 0), roll, crown_out,
                               rng.uniform(1.05, 1.3))
                add_spray_card(prim, s0, s1, (0, 1, 0), roll + math.radians(65),
                               crown_out, rng.uniform(0.85, 1.05))
                # Third layer, rolled the other way: the sprig texture is
                # sparse, so depth complexity is what closes the crown.
                add_spray_card(prim, s0, s1, (0, 1, 0), roll - math.radians(55),
                               crown_out, rng.uniform(0.85, 1.05))
        # Inner filler: short cards near the trunk so the crown is not
        # see-through at its core when backlit.
        for _ in range(2):
            az = rng.uniform(0, 2 * math.pi)
            out = (math.cos(az), 0.0, math.sin(az))
            base = trunk_centre(lean, y + rng.uniform(-0.05, 0.05))
            tip = v_add(base, v_mul(v_norm((out[0], rng.uniform(-0.3, 0.2), out[2])),
                                    max(0.12, radius * 0.45)))
            add_spray_card(prim, base, tip, (0, 1, 0), rng.uniform(0, math.pi),
                           v_norm((out[0], 0.4, out[2])), 1.1)
    # Leader: two crossed vertical cards at the top.
    top = trunk_centre(lean, HEIGHT * 0.93)
    tip = v_add(top, (0.0, HEIGHT * 0.09, 0.0))
    for r in (0.0, math.pi / 2):
        add_spray_card(prim, top, tip, (1, 0, 0), r, (0, 1, 0), 0.9)


def write_gltf(prims, materials, images, name="spruce_dense"):
    blob = bytearray()
    buffer_views, accessors, meshes_prims = [], [], []

    def push(data, target, count, comp_type, type_, mn=None, mx=None):
        while len(blob) % 4:
            blob.append(0)
        off = len(blob)
        blob.extend(data)
        buffer_views.append({'buffer': 0, 'byteOffset': off, 'byteLength': len(data),
                             'target': target})
        acc = {'bufferView': len(buffer_views) - 1, 'componentType': comp_type,
               'count': count, 'type': type_}
        if mn is not None:
            acc['min'], acc['max'] = mn, mx
        accessors.append(acc)
        return len(accessors) - 1

    for mat_index, prim in enumerate(prims):
        pos = b''.join(struct.pack('<3f', *p) for p in prim.pos)
        mn = [min(p[i] for p in prim.pos) for i in range(3)]
        mx = [max(p[i] for p in prim.pos) for i in range(3)]
        a_pos = push(pos, 34962, len(prim.pos), 5126, 'VEC3', mn, mx)
        a_nrm = push(b''.join(struct.pack('<3f', *n) for n in prim.nrm), 34962,
                     len(prim.nrm), 5126, 'VEC3')
        a_uv = push(b''.join(struct.pack('<2f', *u) for u in prim.uv), 34962,
                    len(prim.uv), 5126, 'VEC2')
        a_idx = push(b''.join(struct.pack('<I', i) for i in prim.idx), 34963,
                     len(prim.idx), 5125, 'SCALAR')
        meshes_prims.append({'attributes': {'POSITION': a_pos, 'NORMAL': a_nrm,
                                            'TEXCOORD_0': a_uv},
                             'indices': a_idx, 'material': mat_index})

    doc = {
        'asset': {'version': '2.0', 'generator': 'glgen-trees/make_dense_spruce.py'},
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'name': 'SpruceDense', 'mesh': 0}],
        'meshes': [{'name': 'SpruceDense', 'primitives': meshes_prims}],
        'materials': materials,
        'textures': [{'source': i} for i in range(len(images))],
        'images': [{'uri': u} for u in images],
        'buffers': [{'uri': name + '.bin', 'byteLength': len(blob)}],
        'bufferViews': buffer_views,
        'accessors': accessors,
    }
    with open(os.path.join(OUT_DIR, name + '.bin'), 'wb') as f:
        f.write(blob)
    with open(os.path.join(OUT_DIR, name + '.gltf'), 'w', encoding='utf-8') as f:
        json.dump(doc, f, indent=1)


def main():
    rng = random.Random(SEED)
    bark, needles = Prim(), Prim()
    lean = build_trunk(bark, rng)
    build_crown(needles, rng, lean)
    materials = [
        {'name': 'Bark', 'pbrMetallicRoughness': {
            'baseColorTexture': {'index': 0}, 'metallicFactor': 0.0,
            'roughnessFactor': 0.95}},
        {'name': 'Needles', 'alphaMode': 'MASK', 'alphaCutoff': 0.5, 'doubleSided': True,
         'pbrMetallicRoughness': {
             'baseColorTexture': {'index': 1}, 'metallicFactor': 0.0,
             'roughnessFactor': 0.8}},
    ]
    write_gltf([bark, needles], materials, ['bark09.png', 'spruce branch.png'])
    print('bark %d tris, needles %d tris' % (len(bark.idx) // 3, len(needles.idx) // 3))


if __name__ == '__main__':
    main()

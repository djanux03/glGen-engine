"""Bake middle-distance sprays and a scaled photographic far spruce.

Near trees keep the existing dense asset. Middle trees retain the same trunk,
textures and crown dimensions, with fewer, wider branch whorls. Far trees use
the shipped two-plane photographic impostor, scaled to the dense tree's height.
Run: python Tools/glgen-trees/make_woodland_spruce.py
"""
import json
from pathlib import Path
import random
import make_dense_spruce as spruce

ROOT = Path(__file__).resolve().parents[2]


def main():
    spruce.WHORL_SPACING = .68
    original_card = spruce.add_spray_card
    def wide_card(prim, base, tip, up, roll, crown_out, width):
        original_card(prim, base, tip, up, roll, crown_out, width * 1.45)
    spruce.add_spray_card = wide_card
    rng = random.Random(spruce.SEED)
    bark, needles = spruce.Prim(), spruce.Prim()
    lean = spruce.build_trunk(bark, rng)
    spruce.build_crown(needles, rng, lean)
    dense = json.loads((ROOT/'assets/trees/spruce/spruce_dense.gltf').read_text())
    spruce.write_gltf([bark, needles], dense['materials'],
                      ['bark09.png', 'spruce branch.png'], 'woodland_spruce_mid')
    print('woodland_spruce_mid:', (len(bark.idx)+len(needles.idx))//3, 'triangles')
    far = json.loads((ROOT/'assets/trees/spruce_imposter/spruce_imposter.gltf').read_text())
    far['asset']['generator'] = 'make_woodland_spruce.py'
    far['nodes'][0]['scale'] = [5.1/5.698283061385158]*3
    for buffer in far['buffers']: buffer['uri'] = '../spruce_imposter/'+buffer['uri']
    for image in far['images']: image['uri'] = '../spruce_imposter/'+image['uri']
    for material in far['materials']:
        material['doubleSided'] = True
        material['pbrMetallicRoughness']['metallicFactor'] = 0
    (ROOT/'assets/trees/spruce/woodland_spruce_far.gltf').write_text(
        json.dumps(far, separators=(',', ':'))+'\n')


if __name__ == '__main__':
    main()

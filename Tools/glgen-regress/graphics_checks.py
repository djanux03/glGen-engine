"""GPU checks for fog reciprocity, finite shading and occluded local lights.

Run against a private hidden engine, or use woodland_perf.py --graphics-check.
These test rendered behaviour, including shader/UBO/descriptors, rather than
reimplementing the shading equations in a CPU-only test.
"""
import argparse
import json
import math
from pathlib import Path
import statistics
import sys
import time

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'glgen-client'))
from glgen_client import GlGenClient


def run_checks(c, output):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    original = c.call('render.getParams')
    report = {}

    def capture(name, params):
        c.call('render.setParams', params)
        target = c.call('engine.info')['frame'] + 20
        deadline = time.monotonic() + 90
        while c.call('engine.info')['frame'] < target:
            if time.monotonic() > deadline:
                raise TimeoutError('GPU check stopped advancing')
            time.sleep(.1)
        path = output / (name + '.png')
        c.call('render.capture', {'path': str(path), 'maxDimension': 1440})
        return path

    def centre(path, half=8):
        with Image.open(path) as im:
            patch = im.convert('RGB').crop((im.width//2-half, im.height//2-half,
                                           im.width//2+half, im.height//2+half))
            pixels = getattr(patch, 'get_flattened_data', patch.getdata)()
            return statistics.mean(sum(p)/3 for p in pixels)

    def clear():
        c.eval('if graphics_qa then for _,e in ipairs(graphics_qa) do e:destroy() end end graphics_qa={}')

    def box(pos, scale):
        lua_pos = '{'+','.join(str(v) for v in pos)+'}'
        lua_scale = '{'+','.join(str(v) for v in scale)+'}'
        c.eval('local e=world.spawn("__primitive_cube",{pos='+lua_pos+
               ',scale='+lua_scale+',name="__graphics_qa"}) '
               'e:set_material{color={.6,.6,.6},roughness=.65} table.insert(graphics_qa,e)')

    try:
        clear()
        c.call('render.setParams', {'fixedTime':15, 'temporalAA':False,
            'fxaaEnabled':False, 'autoExposure':False, 'exposure':1, 'gamma':1,
            'water':False, 'volumetric':False, 'pointLights':[], 'timeOfDay':False,
            'fogStart':0, 'fogNoiseStrength':0, 'fogAerialStrength':0,
            'fogMaxOpacity':1, 'fogDensity':.02, 'fogHeightFalloff':.08,
            'fogHeightRef':1000, 'debugView':3})
        # Opposite views of one segment crossing the clamped fog floor must
        # have the same optical depth. Thin targets keep endpoints accurate.
        a, b = [0,1010,10], [0,990,-10]
        box(b, [4,.002,4])
        down = capture('fog_descending', {'camPos':a, 'camYaw':180, 'camPitch':-45})
        clear()
        box(a, [4,.002,4])
        up = capture('fog_ascending', {'camPos':b, 'camYaw':0, 'camPitch':45})
        report['fogReciprocityCodes'] = [centre(down), centre(up)]
        assert abs(centre(down)-centre(up)) < 2, 'Height fog is not reciprocal'
        assert 20 < centre(up) < 240, 'Fog check target was missed or saturated'

        # Extreme downward optical depth used to overflow exp(), which then
        # contaminated both surface light and fog with NaNs.
        bad = capture('fog_extreme_nan', {'debugView':6, 'fogDensity':.08,
            'fogHeightFalloff':1, 'fogHeightRef':2000})
        with Image.open(bad) as im:
            rgb = im.convert('RGB')
            pixels = getattr(rgb, 'get_flattened_data', rgb.getdata)()
            invalid = sum(r>200 and g<20 for r,g,b in pixels)
        assert invalid == 0, f'{invalid} non-finite shader pixels'
        report['finiteExtremeFog'] = True

        clear()
        c.call('render.setParams', {'debugView':0, 'fogDensity':0,
            'fogHeightRef':0, 'sunIntensity':0, 'ambientIntensity':0,
            'iblSpecularIntensity':0, 'bloomIntensity':0, 'shadowStrength':1,
            'pointLights':[{'position':[-3,1004,1], 'color':[1,.8,.6],
                            'intensity':80, 'radius':20}]})
        c.eval('local e=world.spawn("__primitive_plane",{pos={0,1000,0},scale={20,1,20},name="__graphics_qa"}) '
               'e:set_material{color={.6,.6,.6},roughness=.65} table.insert(graphics_qa,e)')
        box([-1.5,1002,.5], [1,1,1])
        camera = {'camPos':[0,1009,10], 'camYaw':180, 'camPitch':-math.degrees(math.atan(.9))}
        blocked = capture('point_shadow', camera)
        c.eval('graphics_qa[2]:destroy() table.remove(graphics_qa,2)')
        open_light = capture('point_unoccluded', camera)
        report['pointLightCentreCodes'] = [centre(blocked), centre(open_light)]
        assert centre(open_light)>centre(blocked)+10, 'Local light leaked through its blocker'
        # A blocker behind the emitter must not affect its finite shadow ray.
        box([-4.5,1006,1.5], [1,1,1])
        behind = capture('point_blocker_beyond_light', camera)
        assert abs(centre(behind)-centre(open_light)) < 2, 'Point shadow continued past its emitter'
        report['finiteLightShadowRange'] = True
        report['localLightShadows'] = True
        c.call('render.setParams', {'temporalSharpness':5})
        assert c.call('render.getParams')['temporalSharpness'] == .5
        report['sharpnessBounded'] = True
        c.call('render.setParams', original)
        before_review = c.call('render.getParams')
        reviewed = c.call('render.turntable', {'assetId':'gen://kitbash.v1/prop/crate',
            'path':str(output/'turntable'), 'steps':2})
        assert len(reviewed['paths']) == reviewed['steps'] == 2, 'Turntable emitted extra views'
        after_review = c.call('render.getParams')
        for key in ('exposure','sunYaw','sunPitch','fogDensity','fogMaxOpacity',
                    'camPos','camYaw','camPitch','autoExposure','fixedTime','temporalAA',
                    'atmosphere','pointLights','cloudMaxSteps','cloudLightTaps','cloudDetailScale'):
            assert before_review[key] == after_review[key], f'Turntable changed {key}'
        report['turntableRestoresLighting'] = True
    finally:
        clear()
        c.call('render.setParams', original)
        (output/'checks.json').write_text(json.dumps(report, indent=2)+'\n')
    print('graphicsChecks', json.dumps(report), flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8788)
    parser.add_argument('--output', type=Path, default=Path('captures/graphics_checks'))
    args = parser.parse_args()
    with GlGenClient(port=args.port, timeout=120) as client:
        run_checks(client, args.output)

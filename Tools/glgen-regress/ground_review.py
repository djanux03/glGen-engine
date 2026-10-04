"""Fixed close views of the woodland verge, grass and ground litter.

Starts its own hidden runtime. --asset-regression also checks recipe geometry
and shading in that same process. Use separate output folders for before/after.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'Tools/glgen-client'))
from glgen_client import GlGenClient
from regress import read_png


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'captures/ground_review')
    parser.add_argument('--asset-regression', action='store_true')
    parser.add_argument('--shadow-check', action='store_true',
                        help='Verify stock grass stays out of the TLAS with either master switch value')
    parser.add_argument('--ao-check', action='store_true',
                        help='Compare identical ground views with SSAO off/on')
    parser.add_argument('--profile', default='woodland_swamp')
    parser.add_argument('--coverage-only', action='store_true',
                        help='Review meadow coverage from high, middle and walking views')
    parser.add_argument('--diagnostics', action='store_true',
                        help='Review close ground shading, albedo, normals and AA')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    with tempfile.NamedTemporaryFile(mode='w', suffix='.lua', delete=False) as noop:
        noop.write('-- Skip editor session restoration for reproducible review.\n')
    env = os.environ.copy()
    env.update(GLGEN_BACKGROUND='1', GLGEN_BACKGROUND_FPS='15',
               GLGEN_SCENERY=args.profile, GLGEN_TERRAIN_ON_START='1',
               GLGEN_SCRIPT=noop.name, GLGEN_FIXED_TIME='15',
               GLGEN_SMOKE_FRAMES='12000', GLGEN_SMOKE_CAM='-25,6,17,-35,95',
               GLGEN_AGENT_PORT=str(port))
    proc = None
    try:
        with (output/'runtime.log').open('w') as log:
            proc = subprocess.Popen([str(ROOT/'Build-vs18/bin/Release/glGenVk.exe')],
                                    cwd=ROOT, env=env, stdout=log, stderr=log,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            deadline = time.monotonic()+120
            while True:
                try:
                    client = GlGenClient(port=port, timeout=120)
                    break
                except OSError:
                    if proc.poll() is not None or time.monotonic()>deadline: raise
                    time.sleep(.2)
            with client as c:
                report = {'rendererParams': c.call('render.getParams')}
                report['terrainSettings'] = c.eval('return terrain.settings()')
                print('terrain settings:',json.dumps(report['terrainSettings']),flush=True)
                assert not report['terrainSettings']['grassCastShadows'], 'Grass blades should not cast shadows'
                if args.coverage_only:
                    # Review local vegetation after streaming settles, without
                    # spending the run uploading the 40-chunk alpine panorama.
                    assert c.eval('return terrain.regenerate{viewDistanceChunks=8}')
                views = [
                    ('coverage_detail',-28,23,3.6,125,-68),
                    ('coverage_high',-28,23,7,125,-55),
                    ('coverage_middle',-28,23,3.8,125,-30),
                    ('coverage_walking',-28,23,1.7,125,-12)]
                if not args.coverage_only: views += [
                    ('verge',-25,17,2.2,95,-48),
                    ('sticks',-25,17,1.3,95,-65),
                    ('grass',-28,23,1.05,125,-28),
                    ('litter',-48,36,1.35,125,-48),
                    ('track',-25,17,1.7,95,-10),
                    ('meadow',-28,23,1.75,125,-10),
                    ('marsh',-188,-51,1.8,150,-12),
                    ('marsh_ground',-188,-51,1.1,150,-55)]
                if args.diagnostics:
                    views = [('close_ground',-28,23,.9,125,-65)]
                report['profile'] = args.profile
                for name,x,z,height,yaw,pitch in views:
                    y = c.eval(f'return terrain.height_at({x},{z})')+height
                    c.call('render.setParams', {'camPos':[x,y,z], 'camYaw':yaw, 'camPitch':pitch})
                    target = c.call('engine.info')['frame']+120
                    while c.call('engine.info')['frame']<target: time.sleep(.1)
                    c.call('render.capture', {'path':str(output/f'{name}.png'), 'maxDimension':1440})
                    samples = [c.eval('return render.stats()') for _ in range(20)]
                    report[name] = {k:statistics.median(s[k] for s in samples)
                                    for k,v in samples[0].items() if isinstance(v,(int,float))}
                    print(name, json.dumps(report[name]), flush=True)
                if args.diagnostics:
                    for name, params in [('albedo', {'debugView':1}),
                        ('normals', {'debugView':2}), ('ao', {'debugView':4}),
                        ('no_aa', {'debugView':0,'temporalAA':False,'fxaaEnabled':False}),
                        ('aa', {'debugView':0,'temporalAA':True,'fxaaEnabled':True})]:
                        c.call('render.setParams',params)
                        target=c.call('engine.info')['frame']+32
                        while c.call('engine.info')['frame']<target: time.sleep(.1)
                        c.call('render.capture',{'path':str(output/f'{name}.png'),
                                                'maxDimension':1440})
                if args.ao_check:
                    ao = {}
                    x,z = -25,17
                    y = c.eval(f'return terrain.height_at({x},{z})')+1.3
                    for strength in (0,report['rendererParams']['aoStrength']):
                        c.call('render.setParams', {'camPos':[x,y,z], 'camYaw':95,
                                                   'camPitch':-65,'aoStrength':strength})
                        target = c.call('engine.info')['frame']+120
                        while c.call('engine.info')['frame']<target: time.sleep(.1)
                        name = 'ao_on' if strength else 'ao_off'
                        path = output/f'{name}.png'
                        c.call('render.capture', {'path':str(path),'maxDimension':1440})
                        w,h,pixels = read_png(str(path))
                        ao[name] = {'meanRgb':sum(pixels[i] for i in range(len(pixels)) if i%4<3)/(w*h*3),
                                    'strength':strength}
                    assert ao['ao_on']['meanRgb'] < ao['ao_off']['meanRgb']-.1, 'SSAO has no visible effect'
                    report['aoCheck'] = ao
                    print('SSAO:',json.dumps(ao),flush=True)
                if args.shadow_check:
                    shadow = {}
                    x,z = -25,17
                    y = c.eval(f'return terrain.height_at({x},{z})')+2.2
                    for enabled in (True,False):
                        assert c.eval('return terrain.regenerate{grassCastShadows=' +
                                      ('true' if enabled else 'false') + '}')
                        c.call('render.setParams', {'camPos':[x,y,z], 'camYaw':95,'camPitch':-48})
                        target = c.call('engine.info')['frame']+240
                        while c.call('engine.info')['frame']<target: time.sleep(.1)
                        c.call('render.capture', {'path':str(output/f'shadow_{int(enabled)}.png'),'maxDimension':1440})
                        samples = [c.eval('return render.stats()') for _ in range(20)]
                        shadow[str(enabled)] = {k:statistics.median(s[k] for s in samples)
                                                for k,v in samples[0].items() if isinstance(v,(int,float))}
                    # Layer opt-outs are authoritative even if a saved editor
                    # setting re-enables the master grass-shadow checkbox.
                    assert shadow['True']['tlasInstances'] == shadow['False']['tlasInstances']
                    report['grassShadowCheck'] = shadow
                    print('grass shadows:',json.dumps(shadow),flush=True)
                if args.asset_regression:
                    with (output/'asset-regression.log').open('w') as regression_log:
                        result = subprocess.run([sys.executable,str(ROOT/'Tools/glgen-regress/regress.py'),
                            '--port',str(port)], cwd=ROOT, stdout=regression_log, stderr=subprocess.STDOUT)
                    report['assetRegressionExitCode'] = result.returncode
                    print('asset regression exit:',result.returncode,flush=True)
                (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=30)
        Path(noop.name).unlink(missing_ok=True)
    errors = (output/'runtime.log').read_text(errors='replace').count('[Vulkan][ERROR]')
    assert errors == 0, 'Vulkan errors; inspect runtime.log'


if __name__ == '__main__': main()

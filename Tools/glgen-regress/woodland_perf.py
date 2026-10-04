"""Measure woodland GPU passes and capture two fixed views without taking focus.

Run after building Release: python Tools/glgen-regress/woodland_perf.py
--restore-scene also exercises legacy scene migration. Background pacing defaults
to 15 FPS to leave GPU time for other applications; GPU timestamps exclude pacing.
Live games can preempt GPU execution, so compare runs under the same load.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--restore-scene', action='store_true')
    parser.add_argument('--visual-review', action='store_true',
                        help='Also capture open sky and a close shoreline view')
    parser.add_argument('--terrain-review', action='store_true',
                        help='Capture service track, rocky rise and utility corridor')
    parser.add_argument('--dressing-review', action='store_true',
                        help='Close views of shrubs, ground cover and fractured rocks')
    parser.add_argument('--asset-regression', action='store_true',
                        help='Run the recipe regression on the same hidden engine')
    parser.add_argument('--graphics-check', action='store_true',
                        help='GPU checks for fog reciprocity and local light shadows')
    parser.add_argument('--temporal-check', action='store_true',
                        help='Measure consecutive-frame cloud noise with AA off/on')
    parser.add_argument('--sync-validation', action='store_true',
                        help='Enable Vulkan synchronization validation (affects timing)')
    parser.add_argument('--resize-only', action='store_true',
                        help='Skip landscape sampling and exercise hidden target recreation')
    parser.add_argument('--output', type=Path, default=ROOT/'captures/woodland_perf')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Use our own port; never accidentally connect to the user's open editor.
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    noop = tempfile.NamedTemporaryFile(mode='w', suffix='.lua', delete=False)
    noop.write('-- Fixed scenery benchmark; skip session scene restoration.\n')
    noop.close()
    env = os.environ.copy()
    env.update(GLGEN_BACKGROUND='1', GLGEN_BACKGROUND_FPS='15',
               GLGEN_SCENERY='woodland_swamp', GLGEN_TERRAIN_ON_START='1',
               GLGEN_SCRIPT=noop.name, GLGEN_FIXED_TIME='15',
               GLGEN_SMOKE_FRAMES='5000', GLGEN_SMOKE_CAM='22,3.6,-19,-3,190',
               GLGEN_AGENT_PORT=str(port))
    if args.restore_scene:
        env.pop('GLGEN_SCRIPT', None)
    if args.sync_validation:
        env['VK_VALIDATION_VALIDATE_SYNC']='1'
    proc = None
    report = {}
    try:
        with (output/'runtime.log').open('w') as log:
            proc = subprocess.Popen([str(ROOT/'Build-vs18/bin/Release/glGenVk.exe')],
                cwd=ROOT, env=env, stdout=log, stderr=log,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            deadline = time.monotonic()+90
            while True:
                try:
                    client = GlGenClient(port=port, timeout=120)
                    break
                except OSError:
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise
                    time.sleep(.2)
            with client as c:
                if os.name == 'nt':
                    user32 = ctypes.WinDLL('user32')
                    user32.GetForegroundWindow.restype = wt.HWND
                    user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
                    user32.IsWindowVisible.argtypes = [wt.HWND]
                    user32.GetWindowTextW.argtypes = [wt.HWND,wt.LPWSTR,ctypes.c_int]
                    foreground = wt.DWORD()
                    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(foreground))
                    assert foreground.value != proc.pid, 'Engine took foreground focus'
                    visible = []
                    engine_windows = []
                    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
                    def inspect(hwnd, _):
                        pid = wt.DWORD()
                        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                        if pid.value == proc.pid:
                            visible.append(bool(user32.IsWindowVisible(hwnd)))
                            title=ctypes.create_unicode_buffer(256)
                            user32.GetWindowTextW(hwnd,title,len(title))
                            if title.value=='glGen (Vulkan)':engine_windows.append(hwnd)
                        return True
                    user32.EnumWindows(inspect, 0)
                    assert visible and not any(visible), 'Engine window is visible'
                    report['hidden'] = True
                report['startupScene'] = c.eval('return scene.stats()')
                report['rendererParams'] = c.call('render.getParams')
                views=[] if args.resize_only else [
                    ('shore', 22, -19, 1.65, 190, -3, 180),
                    ('marsh', -188, -51, 1.8, 150, -5, 120)]
                if args.terrain_review:
                    views += [('track',-25,17,1.7,95,-10,120),
                              ('rocky_rise',118,10,1.7,145,15,120),
                              ('utility_corridor',-15,43,1.7,110,3,120),
                              ('layout',0,100,160,180,-58,90)]
                if args.dressing_review:
                    views += [('understory',-48,36,1.7,125,-18,120),
                              ('rock_detail',130,2,1.7,165,-12,90),
                              ('fern_bank',52,-21,1.1,250,-16,90)]
                for name, x, z, height, yaw, pitch, warm in views:
                    y = c.eval(f'return terrain.height_at({x},{z})')+height
                    c.call('render.setParams', {'camPos': [x,y,z], 'camYaw': yaw, 'camPitch': pitch})
                    target = c.call('engine.info')['frame']+warm
                    while c.call('engine.info')['frame'] < target:
                        time.sleep(.1)
                    samples = []
                    for _ in range(30):
                        samples.append(c.eval('return render.stats()'))
                        time.sleep(.075)
                    report[name] = {k: statistics.median(s[k] for s in samples)
                                    for k in samples[0] if isinstance(samples[0][k], (int, float))}
                    c.call('render.capture', {'path': str(output/f'{name}.png'), 'maxDimension': 1440})
                    if args.terrain_review and name=='track':
                        c.call('render.setParams',{'temporalAA':False})
                        target=c.call('engine.info')['frame']+30
                        while c.call('engine.info')['frame']<target:time.sleep(.1)
                        c.call('render.capture',{'path':str(output/'track_aa_off.png'),'maxDimension':1440})
                        c.call('render.setParams',{'temporalAA':True})
                    (output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
                    print(name, json.dumps(report[name]), flush=True)
                if args.visual_review:
                    for name, x, z, height, yaw, pitch in [
                        ('open_sky', 22, -19, 18, 215, 24),
                        ('water_edge', 0, -23, 1.4, 180, -4)]:
                        y = c.eval(f'return terrain.height_at({x},{z})')+height
                        c.call('render.setParams', {'camPos':[x,y,z], 'camYaw':yaw, 'camPitch':pitch})
                        target = c.call('engine.info')['frame']+90
                        while c.call('engine.info')['frame']<target:
                            time.sleep(.1)
                        c.call('render.capture', {'path':str(output/f'{name}.png'), 'maxDimension':1440})
                        if args.temporal_check and name=='open_sky':
                            from PIL import Image
                            noise = {}
                            for enabled in (False,True):
                                c.call('render.setParams', {'temporalAA':enabled})
                                target = c.call('engine.info')['frame']+90
                                while c.call('engine.info')['frame']<target:
                                    time.sleep(.1)
                                frames=[]
                                for i in range(5):
                                    path=output/f'temporal_{int(enabled)}_{i}.png'
                                    c.call('render.capture',{'path':str(path),'maxDimension':1440})
                                    with Image.open(path) as image:
                                        # Exclude the horizon/forest: their alpha
                                        # silhouette is a separate signal from sky noise.
                                        frames.append(image.convert('RGB').crop(
                                            (0,0,image.width,image.height*3//4)).tobytes())
                                noise[str(enabled)]=sum(sum(abs(a-b) for a,b in zip(x,y))
                                    /len(x) for x,y in zip(frames,frames[1:]))/4
                            report['temporalCloudNoise']=noise
                            print('temporalCloudNoise',json.dumps(noise),flush=True)
                            assert noise['True']<noise['False']*.8,'AA did not reduce cloud noise'
                # Scene snapshots must preserve terrain-owned transient entities,
                # while avoiding persisted collider multiplication on the next boot.
                saved = output/'authored_scene.json'
                assert c.eval('return scene.save('+json.dumps(saved.as_posix())+')')
                entries = json.loads(saved.read_text())['entities']
                assert not any(e.get('name') in ('CollidableRock', 'InteractiveTree')
                               or e.get('name', '').startswith('TerrainChunk_') for e in entries)
                count = c.eval('return scene.stats().entities')
                assert c.eval('return scene.load('+json.dumps(saved.as_posix())+')')
                if not args.resize_only:
                    assert c.eval('return scene.stats().entities') == count
                report['persistedEntities'] = len(entries)
                # A resize-only run skips streaming warmup, so terrain can
                # create entities between RPC calls; compare steady-state runs.
                if not args.resize_only:report['snapshotPreservedLiveTerrain'] = True
                if (args.temporal_check or args.resize_only) and os.name=='nt':
                    # Resize only our hidden window, without activation. This
                    # exercises recreation/rebinding of both history slots.
                    user32.GetClientRect.argtypes=[wt.HWND,ctypes.POINTER(wt.RECT)]
                    user32.GetWindowRect.argtypes=[wt.HWND,ctypes.POINTER(wt.RECT)]
                    user32.SetWindowPos.argtypes=[wt.HWND,wt.HWND,ctypes.c_int,ctypes.c_int,
                                                  ctypes.c_int,ctypes.c_int,ctypes.c_uint]
                    assert engine_windows,'GLFW render window not found'
                    hwnd=engine_windows[0]
                    client_rect,outer=wt.RECT(),wt.RECT()
                    user32.GetClientRect(hwnd,ctypes.byref(client_rect))
                    user32.GetWindowRect(hwnd,ctypes.byref(outer))
                    width=960+(outer.right-outer.left)-(client_rect.right-client_rect.left)
                    height=540+(outer.bottom-outer.top)-(client_rect.bottom-client_rect.top)
                    assert user32.SetWindowPos(hwnd,None,0,0,width,height,0x0010|0x0004|0x0002)
                    target=c.call('engine.info')['frame']+60
                    while c.call('engine.info')['frame']<target:time.sleep(.1)
                    path=output/'resized.png'
                    c.call('render.capture',{'path':str(path),'maxDimension':1440})
                    from PIL import Image
                    with Image.open(path) as image:report['resizedCapture']=list(image.size)
                    assert report['resizedCapture']==[960,540],'Swapchain did not resize'
                    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(),ctypes.byref(foreground))
                    assert foreground.value!=proc.pid and not user32.IsWindowVisible(hwnd)
                if args.asset_regression:
                    with (output/'asset-regression.log').open('w') as regression_log:
                        result = subprocess.run([sys.executable,
                            str(ROOT/'Tools/glgen-regress/regress.py'), '--port',str(port)],
                            cwd=ROOT, stdout=regression_log, stderr=subprocess.STDOUT,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
                    report['assetRegressionExitCode'] = result.returncode
                if args.graphics_check:
                    from graphics_checks import run_checks
                    report['graphicsChecks'] = run_checks(c, output/'graphics_checks')
                (output/'results.json').write_text(json.dumps(report, indent=2)+'\n')
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=30)
        Path(noop.name).unlink(missing_ok=True)
    report['vulkanValidationErrors']=(output/'runtime.log').read_text(errors='replace').count('[Vulkan][ERROR]')
    (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    if args.sync_validation:
        assert report['vulkanValidationErrors']==0,'Vulkan validation reported resource hazards; see runtime.log'


if __name__ == '__main__':
    main()

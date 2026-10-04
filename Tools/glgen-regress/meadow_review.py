"""Review mixed meadow grass and its cheap terrain contact shade in a hidden runtime.

Pins time/exposure and compares the same frame with only the terrain contact
mask toggled. --hold keeps the command port alive for inspecting asset captures;
write STOP in the output directory to close it (ten-minute maximum).
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


def settle(c, frames=90):
    target = c.call('engine.info')['frame'] + frames
    deadline = time.monotonic() + 120
    while c.call('engine.info')['frame'] < target:
        if time.monotonic() > deadline:
            raise TimeoutError('Runtime did not advance')
        time.sleep(.1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'captures/meadow_variants')
    parser.add_argument('--asset-regression', action='store_true')
    parser.add_argument('--hold', action='store_true')
    parser.add_argument('--resize-check', action='store_true')
    parser.add_argument('--skip-meadow-views', action='store_true',
                        help='Skip camera views already reviewed during a resize/asset follow-up')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output/'STOP').unlink(missing_ok=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    with tempfile.NamedTemporaryFile(mode='w', suffix='.lua', delete=False) as noop:
        noop.write('-- Skip editor session restoration for reproducible review.\n')
    env = os.environ.copy()
    env.update(GLGEN_BACKGROUND='1', GLGEN_BACKGROUND_FPS='15',
               GLGEN_SCENERY='woodland_swamp', GLGEN_TERRAIN_ON_START='1',
               GLGEN_SCRIPT=noop.name, GLGEN_FIXED_TIME='15',
               GLGEN_SMOKE_FRAMES='18000', GLGEN_SMOKE_CAM='-25,6,17,-35,95',
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
                report = {'port':port, 'pid':proc.pid,
                          'terrainSettings':c.eval('return terrain.settings()')}
                assert not report['terrainSettings']['grassCastShadows']
                assert c.eval('return terrain.regenerate{viewDistanceChunks=8}')
                c.call('render.setParams', {'autoExposure':False,'exposure':1.1})
                views=[] if args.skip_meadow_views else [
                    ('meadow',1.7,-12),('mixed_blades',1.05,-28),('ground_contact',3.6,-68)]
                for name,height,pitch in views:
                    x,z=-28,23
                    y=c.eval(f'return terrain.height_at({x},{z})')+height
                    c.call('render.setParams', {'camPos':[x,y,z],'camYaw':125,'camPitch':pitch})
                    settle(c,150)
                    c.call('render.capture',{'path':str(output/f'{name}.png'),'maxDimension':1440})
                    samples=[c.eval('return render.stats()') for _ in range(20)]
                    report[name]={k:statistics.median(s[k] for s in samples)
                                  for k,v in samples[0].items() if isinstance(v,(int,float))}
                    print(name,json.dumps(report[name]),flush=True)
                # Streaming may update the draw radius. Wait for uploads to
                # stop before disabling that one uniform for a clean A/B.
                stable=0
                previous=None
                deadline=time.monotonic()+120
                while stable<8:
                    slots=c.eval('return render.stats().meshSlotsLive')
                    stable=stable+1 if slots==previous else 0
                    previous=slots
                    if time.monotonic()>deadline: raise TimeoutError('Terrain did not settle')
                    time.sleep(.5)
                radius=c.call('render.getParams')['grassGroundDrawDistance']
                assert radius>0
                c.call('render.setParams',{'temporalAA':False})
                comparison={}
                for name,value in [('contact_on',radius),('contact_off',0)]:
                    c.call('render.setParams',{'grassGroundDrawDistance':value})
                    settle(c,45)
                    assert c.call('render.getParams')['grassGroundDrawDistance']==value
                    path=output/f'{name}.png'
                    c.call('render.capture',{'path':str(path),'maxDimension':1440})
                    w,h,pixels=read_png(str(path))
                    comparison[name]=sum(pixels[i] for i in range(len(pixels)) if i%4<3)/(w*h*3)
                assert comparison['contact_on']<comparison['contact_off']-.1, 'Ground contact shade is invisible'
                report['contactMeanRgb']=comparison
                print('contact comparison:',json.dumps(comparison),flush=True)
                c.call('render.setParams',{'grassGroundDrawDistance':radius,'temporalAA':True})
                # A lighting-independent view distinguishes actual contact
                # pockets from a broad change in ground colour/exposure.
                c.call('render.setParams',{'debugView':4})
                settle(c,32)
                c.call('render.capture',{'path':str(output/'contact_occlusion.png'),'maxDimension':1440})
                c.call('render.setParams',{'debugView':0})
                if args.resize_check and os.name=='nt':
                    # Exercise both AO targets at a new size using only our
                    # hidden render window; never activate the user's apps.
                    import ctypes
                    from ctypes import wintypes as wt
                    user32=ctypes.windll.user32
                    user32.GetWindowThreadProcessId.argtypes=[wt.HWND,ctypes.POINTER(wt.DWORD)]
                    user32.GetWindowTextW.argtypes=[wt.HWND,wt.LPWSTR,ctypes.c_int]
                    user32.IsWindowVisible.argtypes=[wt.HWND]
                    windows=[]
                    callback_type=ctypes.WINFUNCTYPE(wt.BOOL,wt.HWND,wt.LPARAM)
                    def collect(hwnd,_):
                        owner=wt.DWORD()
                        user32.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
                        if owner.value==proc.pid:
                            title=ctypes.create_unicode_buffer(256)
                            user32.GetWindowTextW(hwnd,title,len(title))
                            if title.value=='glGen (Vulkan)':windows.append(hwnd)
                        return True
                    callback=callback_type(collect)
                    user32.EnumWindows(callback,0)
                    assert windows, 'Hidden render window not found'
                    hwnd=windows[0]
                    user32.GetClientRect.argtypes=[wt.HWND,ctypes.POINTER(wt.RECT)]
                    user32.GetWindowRect.argtypes=[wt.HWND,ctypes.POINTER(wt.RECT)]
                    user32.SetWindowPos.argtypes=[wt.HWND,wt.HWND,ctypes.c_int,ctypes.c_int,
                                                  ctypes.c_int,ctypes.c_int,ctypes.c_uint]
                    client_rect,outer=wt.RECT(),wt.RECT()
                    user32.GetClientRect(hwnd,ctypes.byref(client_rect))
                    user32.GetWindowRect(hwnd,ctypes.byref(outer))
                    borders=(outer.right-outer.left-client_rect.right,
                             outer.bottom-outer.top-client_rect.bottom)
                    original=(client_rect.right,client_rect.bottom)
                    for name,size in [('resized',(960,540)),('restored',original)]:
                        assert user32.SetWindowPos(hwnd,None,0,0,size[0]+borders[0],
                                                  size[1]+borders[1],0x0010|0x0004|0x0002)
                        settle(c,60)
                        path=output/f'{name}.png'
                        c.call('render.capture',{'path':str(path),'maxDimension':1440})
                        assert read_png(str(path))[:2]==size
                        assert not user32.IsWindowVisible(hwnd)
                    report['resizeCheck']='passed'
            if args.asset_regression:
                with (output/'asset-regression.log').open('w') as reglog:
                    result=subprocess.run([sys.executable,str(ROOT/'Tools/glgen-regress/regress.py'),
                        '--port',str(port)],cwd=ROOT,stdout=reglog,stderr=subprocess.STDOUT)
                report['assetRegressionExitCode']=result.returncode
                print('asset regression exit:',result.returncode,flush=True)
            (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
            print('Review ready; port',port,flush=True)
            if args.hold:
                deadline=time.monotonic()+600
                while not (output/'STOP').exists() and time.monotonic()<deadline and proc.poll() is None:
                    time.sleep(.5)
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=30)
        Path(noop.name).unlink(missing_ok=True)
    assert '[Vulkan][ERROR]' not in (output/'runtime.log').read_text(errors='replace')


if __name__=='__main__': main()

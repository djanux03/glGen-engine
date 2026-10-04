"""Exercise AK gameplay and capture its poses in a hidden, muted engine.

Run after a Release build. --woodland also captures the gun in the landscape.
--asset-regression runs the existing recipe/golden check before shutdown.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Tools/glgen-client'))
from glgen_client import GlGenClient


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'captures/rifle')
    parser.add_argument('--sync-validation', action='store_true')
    parser.add_argument('--woodland', action='store_true')
    parser.add_argument('--asset-regression', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    startup = tempfile.NamedTemporaryFile(mode='w', suffix='.lua', delete=False)
    start_position = '{22,1.62,-19}' if args.woodland else '{0,1.62,0}'
    if not args.woodland:
        # A scene without terrain needs real collision ground. The controller
        # must not invent a floor at y=0 for imported/editor scenes.
        floor = output / 'floor.json'
        floor.write_text(json.dumps({'entities': [{
            'id': 1, 'name': 'Test floor',
            'transform': {'position': [0, -.5, 0], 'scale': [1, 1, 1]},
            'collider': {'shape': 'Box', 'dimensions': [100, 1, 100]},
            'rigidbody': {'type': 'Static'},
        }]}))
        startup.write('scene.load(' + json.dumps(floor.as_posix()) + ')\n')
    startup.write('''render.params{camPos={0,1.62,0},camYaw=180,camPitch=0,fov=65,
      autoExposure=false,exposure=1,temporalAA=true,stylizedLightingRamp=0,
      sunIntensity=3,ambientIntensity=1,saturation=1}
      player=game.spawn_player{pos=START_POSITION,yaw=0,pitch=0,onGround=true}
      assert(weapon.equip(player))
      weapon.control{trigger=true}
      game.play()
    '''.replace('START_POSITION', start_position))
    startup.close()
    env = os.environ.copy()
    env.update(GLGEN_BACKGROUND='1', GLGEN_BACKGROUND_FPS='15', GLGEN_MUTE='1',
               GLGEN_SCRIPT=startup.name, GLGEN_FIXED_TIME='15', GLGEN_AGENT_PORT=str(port))
    env.pop('GLGEN_SMOKE_FRAMES', None)
    if args.woodland:
        env.update(GLGEN_SCENERY='woodland_swamp', GLGEN_TERRAIN_ON_START='1')
    else:
        env.pop('GLGEN_TERRAIN_ON_START', None)
        env.pop('GLGEN_SCENERY', None)
    if args.sync_validation:
        env['VK_VALIDATION_VALIDATE_SYNC'] = '1'
    report = {}
    proc = None
    try:
        with (output / 'runtime.log').open('w') as log:
            proc = subprocess.Popen([str(ROOT / 'Build-vs18/bin/Release/glGenVk.exe')],
                cwd=ROOT, env=env, stdout=log, stderr=log,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            deadline = time.monotonic() + 100
            while True:
                try:
                    client = GlGenClient(port=port, timeout=120)
                    break
                except OSError:
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('Hidden engine failed to start; see runtime.log')
                    time.sleep(.2)
            with client as c:
                if os.name == 'nt':
                    user32 = ctypes.WinDLL('user32')
                    user32.GetForegroundWindow.restype = wt.HWND
                    user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
                    user32.IsWindowVisible.argtypes = [wt.HWND]
                    user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
                    foreground = wt.DWORD()
                    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(foreground))
                    assert foreground.value != proc.pid, 'Engine took focus'
                    visible = []
                    engine_windows = []
                    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
                    def inspect(hwnd, _):
                        pid = wt.DWORD()
                        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                        if pid.value == proc.pid:
                            visible.append(bool(user32.IsWindowVisible(hwnd)))
                            title = ctypes.create_unicode_buffer(256)
                            user32.GetWindowTextW(hwnd, title, len(title))
                            if title.value == 'glGen (Vulkan)':
                                engine_windows.append(hwnd)
                        return True
                    user32.EnumWindows(inspect, 0)
                    assert visible and not any(visible), 'Engine window is visible'
                    report['hidden'] = True

                def state():
                    return c.eval('return weapon.state()')

                def wait_for(predicate, seconds=8):
                    deadline = time.monotonic() + seconds
                    while time.monotonic() < deadline:
                        current = state()
                        if predicate(current):
                            return current
                        time.sleep(.05)
                    raise AssertionError(f'Weapon state timed out: {state()}')

                def capture(name):
                    c.call('render.capture', {'path': str(output / (name + '.png')), 'maxDimension': 1440})

                time.sleep(3)
                assert state()['magazine'] == 30, 'Held Play click fired before release'
                c.eval('weapon.control{}')
                capture('hip')
                c.eval('weapon.control{aim=true}')
                wait_for(lambda s: s['aim'] > .99)
                capture('aim')
                camera = c.call('render.getParams')['camPos']
                # Large target health preserves impact marks for the capture.
                target = c.eval('target=weapon.target{pos={' + ','.join(map(str,
                    [camera[0], camera[1], camera[2] - 12])) + '},health=100};return target')
                time.sleep(.5)
                c.eval('weapon.mode(false);weapon.control{trigger=true,aim=true}')
                wait_for(lambda s: s['shots'] == 1)
                time.sleep(.3)
                assert state()['shots'] == 1, 'Semi fired on a held trigger'
                assert c.eval('return weapon.health(target)') == 66, 'First shot did not damage target'
                capture('impact')
                for shots in (2, 3):
                    c.eval('weapon.control{aim=true}')
                    time.sleep(.15)
                    c.eval('weapon.control{trigger=true,aim=true}')
                    wait_for(lambda s: s['shots'] == shots)
                assert c.eval('return weapon.health(target)') == 0, 'Three hits did not destroy target'
                c.eval('weapon.control{};weapon.reload()')
                wait_for(lambda s: .34 < s['reloadProgress'] < .48)
                during = state()
                assert during['magazine'] == 27 and during['reserve'] == 120
                capture('reload')
                done = wait_for(lambda s: not s['reloading'])
                assert done['magazine'] == 30 and done['reserve'] == 117
                report['tacticalReload'] = done
                automatic_start = time.monotonic()
                c.eval('weapon.mode(true);weapon.control{trigger=true}')
                # Bridge handlers run after gameplay: request the same frame
                # that actually fired, rather than the frame setting the input.
                flash_path = json.dumps((output / 'flash.png').as_posix())
                deadline = time.monotonic() + 2
                while not c.eval('if weapon.state().shotAge<.02 then return render.capture('
                                 + flash_path + ') end;return false'):
                    assert time.monotonic() < deadline, 'No muzzle flash frame'
                    time.sleep(.02)
                wait_for(lambda s: s['shots'] >= 8)
                capture('firing')
                done = wait_for(lambda s: s['magazine'] == 0)
                report['automaticEmpty'] = done
                report['automaticMagazineSeconds'] = time.monotonic() - automatic_start
                assert done['shots'] == 33, 'Auto fire consumed an incorrect number of rounds'
                time.sleep(.3)
                assert state()['shots'] == 33, 'Empty magazine fired'
                c.eval('weapon.control{};weapon.reload()')
                wait_for(lambda s: .76 < s['reloadProgress'] < .88)
                capture('charging')
                done = wait_for(lambda s: not s['reloading'])
                assert done['magazine'] == 30 and done['reserve'] == 87
                report['emptyReload'] = done
                report['renderer'] = c.eval('return render.stats()')
                if os.name == 'nt':
                    assert engine_windows, 'Cannot locate hidden engine window'
                    user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int,
                        ctypes.c_int, ctypes.c_int, ctypes.c_int, wt.UINT]
                    user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
                    user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
                    outer, inner = wt.RECT(), wt.RECT()
                    user32.GetWindowRect(engine_windows[0], ctypes.byref(outer))
                    user32.GetClientRect(engine_windows[0], ctypes.byref(inner))
                    border_w = outer.right - outer.left - inner.right
                    border_h = outer.bottom - outer.top - inner.bottom
                    for width, height in ((960, 540), (1280, 720)):
                        # Resize only this process's hidden window, without activation.
                        assert user32.SetWindowPos(engine_windows[0], None, 0, 0,
                            width + border_w, height + border_h, 0x0002 | 0x0004 | 0x0010)
                        time.sleep(.8)
                        capture('resized')
                        assert struct.unpack('>II', (output / 'resized.png').read_bytes()[16:24]) == (width, height)
                    report['resizedCapture'] = struct.unpack('>II',
                        (output / 'resized.png').read_bytes()[16:24])
                    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(foreground))
                    assert foreground.value != proc.pid, 'Resize took focus'
                saved = output / 'rifle_scene.json'
                assert c.eval('return scene.save(' + json.dumps(saved.as_posix()) + ')')
                entries = json.loads(saved.read_text())['entities']
                report['savedRifle'] = [e for e in entries if 'rifle' in e]
                assert len(report['savedRifle']) == 1
                c.eval('game.stop()')
                time.sleep(.5)
                c.eval('game.play();weapon.control{}')
                time.sleep(.5)
                assert state()['magazine'] == 30 and state()['reserve'] == 120, 'Stop failed to restore loadout'
                c.eval('game.stop()')
                if args.asset_regression:
                    time.sleep(.5)
                    with (output / 'asset-regression.log').open('w') as regression:
                        result = subprocess.run([sys.executable, str(ROOT / 'Tools/glgen-regress/regress.py'),
                            '--port', str(port)], cwd=ROOT, stdout=regression, stderr=regression)
                    report['assetRegressionExit'] = result.returncode
                print(json.dumps(report, indent=2), flush=True)
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=20)
        Path(startup.name).unlink(missing_ok=True)
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    log = (output / 'runtime.log').read_text(errors='replace')
    assert 'Validation Error' not in log and 'SYNC-HAZARD' not in log, 'Vulkan validation failed'


if __name__ == '__main__':
    main()

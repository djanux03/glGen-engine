"""Verify legacy player recovery, upright look and repeated Play/Stop, hidden."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Tools/glgen-client'))
from glgen_client import GlGenClient


def main():
    output = ROOT / 'captures/spawn-fix'
    output.mkdir(parents=True, exist_ok=True)
    # The exact old offset/scale and distant position reported by the user.
    player = {'name': 'Player', 'id': 1,
        'transform': {'position': [-113.226, -104.534, 1479.248],
                      'rotation': [0, 77.52, 0], 'scale': [2.007, 5.224, 1]},
        'camera': {'isPrimary': True},
        'collider': {'shape': 'Capsule', 'dimensions': [.6, 1.8, .6], 'offset': [0, 10, 0]},
        'rigidbody': {'type': 'Dynamic', 'mass': 70},
        'rifle': {'enabled': True, 'magazine': 30, 'reserve': 120}}
    legacy = output / 'legacy_player.json'
    legacy.write_text(json.dumps({'entities': [player]}))
    script = tempfile.NamedTemporaryFile(mode='w', suffix='.lua', delete=False)
    script.write('assert(scene.load(' + json.dumps(legacy.as_posix()) + '))\n'
        'render.params{camPos={22,terrain.height_at(22,-19)+1.62,-19},camYaw=190,camPitch=0}\n'
        'weapon.control{}\ngame.play()\n')
    script.close()
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    env = os.environ.copy()
    env.update(GLGEN_BACKGROUND='1', GLGEN_MUTE='1', GLGEN_BACKGROUND_FPS='15',
        GLGEN_SCENERY='woodland_swamp', GLGEN_TERRAIN_ON_START='1', GLGEN_FIXED_TIME='15',
        GLGEN_SCRIPT=script.name, GLGEN_AGENT_PORT=str(port))
    report = {}
    proc = None
    try:
        with (output / 'runtime.log').open('w') as log:
            proc = subprocess.Popen([str(ROOT / 'Build-vs18/bin/Release/glGenVk.exe')],
                cwd=ROOT, env=env, stdout=log, stderr=log,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            deadline = time.monotonic() + 90
            while True:
                try:
                    client = GlGenClient(port=port, timeout=120)
                    break
                except OSError:
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('Hidden engine failed; see runtime.log')
                    time.sleep(.2)
            with client as c:
                def sample():
                    p = c.eval('local p=world.find_entity("Player"):get_position();return {p.x,p.y,p.z}')
                    ground = c.eval(f'return terrain.height_at({p[0]},{p[2]})')
                    assert abs(p[0]) < 520 and abs(p[2]) < 520, f'Spawn outside map: {p}'
                    assert abs(p[1] - ground - 1.62) < .12, f'Player below/above ground: {p}, ground={ground}'
                    return {'position': p, 'ground': ground, 'eyeHeight': p[1] - ground}
                time.sleep(5)
                report['recovered'] = sample()
                c.call('render.capture', {'path': str(output / 'recovered.png'), 'maxDimension': 1440})
                for pitch in (80, -80, 0):
                    c.eval(f'world.find_entity("Player"):set_rotation({pitch},10,0)')
                    time.sleep(1.5)
                    report[f'pitch{pitch}'] = sample()
                for i in range(3):
                    c.eval('game.stop()')
                    time.sleep(.4)
                    c.eval('game.play()')
                    time.sleep(1)
                    report[f'replay{i}'] = sample()
                c.call('render.capture', {'path': str(output / 'replay.png'), 'maxDimension': 1440})
                c.eval('game.stop()')
                time.sleep(.4)
                normalized = output / 'normalized_player.json'
                assert c.eval('return scene.save(' + json.dumps(normalized.as_posix()) + ')')
                entries = json.loads(normalized.read_text())['entities']
                p = next(e for e in entries if e.get('name') == 'Player')
                assert p['rigidbody']['lockRotation']
                assert p['transform']['scale'] == [1, 1, 1]
                assert abs(p['collider']['offset'][1] + .72) < .001
                report['normalizedPlayer'] = p
    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=20)
        Path(script.name).unlink(missing_ok=True)
        (output / 'results.json').write_text(json.dumps(report, indent=2))
    assert 'Validation Error' not in (output / 'runtime.log').read_text(errors='replace')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()

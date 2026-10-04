"""Capture winter scenery and verify reversible switching on a running engine.
Start with GLGEN_TERRAIN_ON_START=1 GLGEN_FIXED_TIME=100 GLGEN_AGENT_PORT=8788.
Run: python Tools/glgen-regress/scenery.py --port 8788
Does not update goldens or save editor settings.
"""
import argparse
import json
from pathlib import Path
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"glgen-client"))
from glgen_client import GlGenClient


def main():
    ap=argparse.ArgumentParser();ap.add_argument("--port",type=int,default=8788)
    ap.add_argument("--output",default="captures/winter");args=ap.parse_args()
    out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True)
    c=GlGenClient(port=args.port,timeout=120)
    def lua(code):return c.call("script.eval",{"code":code})["value"]
    def settle(n=120):
        target=c.call("engine.info")["frame"]+n
        deadline=time.monotonic()+120
        while c.call("engine.info")["frame"]<target:
            if time.monotonic()>deadline:raise TimeoutError("Frame settling timed out")
            time.sleep(.25)
        # Streaming can outlive 120 frames on a large saved view distance.
        # Wait for resident mesh counts to stabilize before comparing images.
        last=None;stable=0
        while stable<6:
            if time.monotonic()>deadline:raise TimeoutError("Terrain did not settle")
            count=lua("return render.stats().meshSlotsLive")
            stable=stable+1 if count==last else 0;last=count
            time.sleep(.5)
    def capture(name,x,z,above,yaw,pitch):
        y=lua(f"return terrain.height_at({x},{z})")+above
        c.call("render.setParams",{"camPos":[x,y,z],"camYaw":yaw,"camPitch":pitch})
        settle()
        c.call("render.capture",{"path":str(out/(name+".png")),"maxDimension":1440})
        stats=lua("return render.stats()")
        start=time.perf_counter();frame=c.call("engine.info")["frame"]
        time.sleep(3)
        frames=c.call("engine.info")["frame"]-frame
        stats["wall_ms_per_frame"]=1000*(time.perf_counter()-start)/max(frames,1)
        stats["camera"]=[x,y,z,yaw,pitch]
        print(name,stats,flush=True)
        return stats
    report={}
    try:
        lua('assert(render.scenery("meadows"))')
        original=c.call("render.getParams")
        report["meadows"]=capture("meadows",0,42,6,180,-8)
        lua('assert(render.scenery("bleak_winter"))')
        report["clearing"]=capture("clearing",0,42,6,180,-8)
        report["forest"]=capture("forest",-18,24,3,150,-5)
        report["valley"]=capture("valley",32,72,55,205,-24)
        # Find dry ground near water and a steep dry face from the engine's
        # actual terrain/water authority; no assumed sea-height thresholds.
        points=lua("local a={} for x=-320,320,32 do for z=-320,320,32 do "
                   "local h=terrain.height_at(x,z) local w=terrain.water_at(x,z) "
                   "local slope=math.abs(terrain.height_at(x+2,z)-h) "
                   "table.insert(a,{x=x,z=z,h=h,w=w,slope=slope}) end end return a")
        shore=min((p for p in points if p["h"]>p["w"] and p["w"]>-1000),key=lambda p:p["h"]-p["w"])
        rock=max((p for p in points if p["h"]>p["w"]+2),key=lambda p:p["slope"])
        report["shore"]=capture("shore",shore["x"],shore["z"],4,180,-14)
        report["rock"]=capture("rock",rock["x"],rock["z"],8,230,-25)
        lua('assert(render.scenery("meadows"))')
        restored=c.call("render.getParams")
        for key in ("sunIntensity","ambientIntensity","autoExposure","sunPitch"):
            assert restored[key]==original[key],(key,original[key],restored[key])
        report["restored"]=capture("restored",0,42,6,180,-8)
    finally:
        (out/"report.json").write_text(json.dumps(report,indent=2))
        c.close()

if __name__=="__main__":main()

"""Renderer QA against a running engine; writes captures without accepting goldens.

Start with GLGEN_TERRAIN_ON_START=1 GLGEN_FIXED_TIME=100 GLGEN_AGENT_PORT=8788.
Run python Tools/glgen-regress/lighting.py --port 8788.
Exercises daylight/twilight/moonlight, shoreline, snow, scalar material overrides,
emission, the FXAA switch, and shader NaN diagnostics. Inspect the PNGs as well.
"""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "glgen-client"))
from glgen_client import GlGenClient
from regress import read_png


def image_stats(path):
    w, h, rgba = read_png(str(path))
    pixels = [rgba[i:i+3] for i in range(0, len(rgba), 4)]
    return {
        "width": w, "height": h,
        "clipped_fraction": sum(max(p) >= 254 for p in pixels) / len(pixels),
        "black_fraction": sum(max(p) <= 2 for p in pixels) / len(pixels),
        "mean_rgb": [sum(p[c] for p in pixels) / len(pixels) for c in range(3)],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8788)
    ap.add_argument("--output", default="captures/renderer-overhaul/final")
    args = ap.parse_args()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {}
    with GlGenClient(port=args.port, timeout=120) as c:
        assert c.eval("return terrain.exists()"), "Start the QA engine with GLGEN_TERRAIN_ON_START=1"
        original = c.call("render.getParams")
        scenery = c.eval("return render.scenery_id()")
        winter_snapshot = None

        def settle(frames=75):
            target = c.call("engine.info")["frame"] + frames
            deadline = time.monotonic() + 120
            while c.call("engine.info")["frame"] < target:
                if time.monotonic() > deadline:
                    raise TimeoutError("Renderer stopped advancing")
                time.sleep(.25)

        def capture(name, params=None):
            if params:
                c.call("render.setParams", params)
            settle()
            path = out / (name + ".png")
            c.call("render.capture", {"path": str(path), "maxDimension": 1440})
            report[name] = image_stats(path)
            report[name]["params"] = c.call("render.getParams")
            print(name, {k:v for k,v in report[name].items() if k != "params"}, flush=True)

        def camera(x, z, above, yaw, pitch, dry=False):
            if dry:
                # Saved worlds can move the coast. Pick nearby dry ground
                # instead of silently running a terrain check underwater.
                sea = c.call("render.getParams")["waterLevel"]
                location = c.eval(f'''
                    local best=nil local score=1e30
                    for iz=-8,8 do for ix=-8,8 do
                      local x,z={x}+ix*64,{z}+iz*64
                      local h=terrain.height_at(x,z)
                      local d=ix*ix+iz*iz
                      if h>{sea}+1 and d<score then best={{x,z}} score=d end
                    end end
                    return best
                ''')
                if location:
                    x, z = location
            h = c.eval(f"return terrain.height_at({x},{z})")
            return {"camPos": [x,h+above,z], "camYaw": yaw, "camPitch": pitch}

        try:
            c.eval('assert(render.scenery("meadows"))')
            c.call("render.setParams", {"fixedTime":100, "timeOfDay":False,
                   "autoExposure":False, "exposure":1.1, "sunPitch":32})
            capture("meadow", camera(0,42,4,0,-5,dry=True))
            capture("sunset", {"sunPitch":5, "exposure":1.5})
            capture("moonlight", {"sunPitch":-25, "exposure":3.2})
            c.call("render.setParams", {"sunPitch":32, "exposure":1.1})
            capture("shore", camera(128,-224,4,180,-14))
            c.eval('assert(render.scenery("bleak_winter"))')
            winter_snapshot = c.call("render.getParams")
            capture("winter", camera(0,42,6,180,-8,dry=True))
            capture("winter_inland", camera(-18,24,4,0,-5,dry=True))
            capture("nan", {"debugView":6})
            # Geometry shaders emit magenta (older versions used red).
            _, _, rgba = read_png(str(out / "nan.png"))
            invalid = sum(rgba[i] > 200 and rgba[i+1] < 20
                          for i in range(0, len(rgba), 4))
            assert invalid == 0, f"{invalid} invalid shader pixels"
            c.call("render.setParams", {"debugView":0})

            # Isolated material chart: shared sphere mesh, per-instance state.
            # It must neither alter all copies nor make new geometry per material.
            c.eval('''
                qa_objects={}
                local function add(asset,pos,scale,material)
                  local e=world.spawn(asset,{pos=pos,scale=scale,name="__lighting_qa"})
                  e:set_material(material) table.insert(qa_objects,e) return e
                end
                add("__primitive_plane",{0,998,0},{18,1,14},
                    {color={.4,.43,.46},roughness=.9})
                for i=0,3 do
                  add("__primitive_sphere",{-4.5+i*3,1000,0},1.6,
                      {color={.7,.55,.32},roughness=(i%2==0 and .18 or .85),
                       metallic=(i>=2 and 1 or 0)})
                end
                qa_glow=add("__primitive_cube",{0,1000,-4},1.2,
                    {color={.06,.06,.06},emissive={1,.25,.04},emissiveStrength=3})
            ''')
            capture("materials", {"camPos":[0,1004,14],"camYaw":180,"camPitch":-15,
                    "sunPitch":32,"sunIntensity":1.6,"ambientIntensity":1,
                    "autoExposure":False,"exposure":1.1,"fogDensity":0,
                    "fogAerialStrength":0,"volumetric":False,"water":False,
                    "snowCoverage":0})
            # Twilight exposes the ambient response and emissive cube.
            capture("materials_ambient", {"shadowStrength":0,"sunPitch":-2})
            c.eval('qa_glow:set_material{emissiveStrength=0}')
            capture("emission_off")
            assert (out / "emission_off.png").read_bytes() != (out / "materials_ambient.png").read_bytes()
            capture("point_light", {"pointLights":[{"position":[0,1002,3],
                    "color":[1,.55,.25],"intensity":65,"radius":16}]})
            assert report["point_light"]["mean_rgb"][0] > report["emission_off"]["mean_rgb"][0]
            c.call("render.setParams", {"pointLights":[]})
            # Temporal resolve handles its own edges/detail recovery; isolate
            # FXAA here so changing its switch exercises the FXAA shader path.
            c.call("render.setParams", {"temporalAA":False})
            c.call("render.setParams", {"fxaaEnabled":False})
            assert c.call("render.getParams")["fxaaEnabled"] is False
            capture("fxaa_off")
            c.call("render.setParams", {"fxaaEnabled":True})
            assert c.call("render.getParams")["fxaaEnabled"] is True
            capture("fxaa_on")
            assert (out / "fxaa_off.png").read_bytes() != (out / "fxaa_on.png").read_bytes()
            report["checks"] = {"finite_shading":True,"emission_changes_image":True,
                                "fxaa_changes_image":True,"point_light_illuminates":True}
        finally:
            c.eval('if qa_objects then for _,e in ipairs(qa_objects) do e:destroy() end end')
            # Scenery switching snapshots outgoing edits. Restore the winter
            # profile before leaving so a second QA run cannot inherit the
            # isolated material chart's lighting or disabled water.
            if winter_snapshot:
                c.call("render.setParams", winter_snapshot)
            c.call("render.setParams", {"pointLights":[]})
            c.eval(f"assert(render.scenery({json.dumps(scenery)}))")
            c.call("render.setParams", original)
            (out / "report.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

"""End-to-end exercise of glGen's command port.

Walks the full loop an MCP client will drive: discover -> read schema ->
generate -> place -> render -> look at the file. Exits non-zero on the first
failure, so it works as a CI check.

    GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe &
    python Tools/glgen-client/smoke.py
"""

from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from glgen_client import GlGenClient, GlGenError  # noqa: E402

PORT = int(os.environ.get("GLGEN_AGENT_PORT", "8787"))
failures = 0


def check(label: str, condition: bool, detail: str = "") -> None:
    global failures
    if condition:
        print(f"  ok    {label}")
    else:
        failures += 1
        print(f"  FAIL  {label}  {detail}")


def connect(retries: int = 40) -> GlGenClient:
    """The engine needs a moment to initialize Vulkan before it listens."""
    for attempt in range(retries):
        try:
            return GlGenClient(port=PORT)
        except OSError:
            if attempt == retries - 1:
                raise
            time.sleep(0.5)
    raise RuntimeError("unreachable")


def main() -> int:
    print(f"connecting to 127.0.0.1:{PORT} ...")
    with connect() as c:
        print("\n[engine.info]")
        info = c.call("engine.info")
        check("engine is glGen", info["engine"] == "glGen", str(info.get("engine")))
        check("renderer is vulkan", info["renderer"] == "vulkan")
        check("generators present", "tree.v1" in info["generators"])
        print(f"  frame={info['frame']} recipes={len(info['recipes'])}")

        print("\n[assets.generators]")
        gens = c.call("assets.generators")
        names = [g["name"] for g in gens["generators"]]
        check("five built-ins", len(names) >= 5, str(names))
        check("descriptions present",
              all(g["description"] for g in gens["generators"]))

        print("\n[assets.schema]")
        schema = c.call("assets.schema", {"name": "rock.v1"})
        props = schema["schema"]["properties"]
        check("radius documented", "radius" in props)
        check("radius has a range",
              "minimum" in props["radius"] and "maximum" in props["radius"])
        check("poly budget reported", schema["polyBudget"] > 0)

        print("\n[assets.define]")
        result = c.define({
            "id": "rock/rpc_boulder",
            "generator": "rock.v1",
            "seed": 424242,
            "params": {"radius": 1.1, "detail": 2, "roughness": 0.45,
                       "flatCuts": 2, "color": [0.46, 0.43, 0.40]},
            "material": {"rock": {"generator": "tex.rock",
                                  "params": {"resolution": 256}}},
        })
        asset_id = result["assetId"]
        check("asset id returned", asset_id.startswith("gen://rock.v1/"), asset_id)
        check("triangles reported", result["triangles"] > 0)
        check("within poly budget", result["triangles"] <= schema["polyBudget"],
              f"{result['triangles']} tris")
        print(f"  {asset_id}  ({result['triangles']} tris)")

        print("\n[assets.define — clamping surfaces as a warning]")
        clamped = c.define({
            "id": "rock/rpc_oversized",
            "generator": "rock.v1",
            "seed": 1,
            "params": {"radius": 999.0},   # schema maximum is 20
        })
        check("warning returned", any("clamped" in w for w in clamped["warnings"]),
              str(clamped["warnings"]))
        print(f"  {clamped['warnings']}")

        print("\n[assets.define — bad params are rejected, not guessed]")
        try:
            c.define({"generator": "rock.v1", "params": {"smooth": "yes please"}})
            check("type error rejected", False, "expected an error")
        except GlGenError as e:
            check("type error rejected", "boolean" in str(e), str(e))

        print("\n[scene.spawn / scene.query]")
        spawned = c.spawn(asset_id, pos=[3.0, 0.0, -4.0], name="RpcBoulder",
                          onGround=True)
        check("entity created", spawned["entityId"] > 0)
        check("dropped onto terrain", spawned["pos"][1] != 0.0,
              str(spawned["pos"]))
        found = c.call("scene.query", {"name": "RpcBoulder"})
        check("query finds it", found["count"] == 1, str(found["count"]))
        check("asset id round-trips",
              found["entities"][0]["assetId"] == asset_id)

        print("\n[script.eval — the escape hatch]")
        gen_list = c.eval("assets.generators()")
        check("lua list returned", "tree.v1" in gen_list, str(gen_list))
        height = c.eval("assets.schema('tree.v1').properties.height.maximum")
        check("nested schema read", height == 40, str(height))
        stats = c.eval("scene.stats()")
        check("scene stats", stats["entities"] > 0, str(stats))
        terrain_y = c.eval("terrain.height_at(0, 0)")
        check("terrain queried", isinstance(terrain_y, (int, float)))

        print("\n[script.eval — errors come back as errors]")
        try:
            c.eval("this is not lua")
            check("lua error reported", False, "expected an error")
        except GlGenError as e:
            check("lua error reported", True, str(e)[:60])

        print("\n[render.setParams / getParams]")
        c.call("render.setParams", {"exposure": 0.85, "sunPitch": 40.0,
                                    "autoExposure": False})
        params = c.call("render.getParams")
        check("exposure applied", abs(params["exposure"] - 0.85) < 1e-4,
              str(params["exposure"]))
        check("sun pitch applied", abs(params["sunPitch"] - 40.0) < 1e-4)

        print("\n[render.capture — deferred until the PNG exists]")
        out = os.path.abspath("captures/rpc_capture.png")
        if os.path.exists(out):
            os.remove(out)
        returned = c.capture(out)
        # The reply is only sent after the frame that wrote the file, so the
        # file must be on disk by the time this line runs.
        check("capture path returned", returned == out, returned)
        check("file exists on reply", os.path.exists(out))
        check("file is non-trivial", os.path.getsize(out) > 10_000,
              f"{os.path.getsize(out) if os.path.exists(out) else 0} bytes")

        print("\n[render.turntable — multi-frame deferral]")
        base = os.path.abspath("captures/rpc_turntable")
        paths = c.turntable(asset_id, base, steps=4)
        check("four shots returned", len(paths) == 4, str(len(paths)))
        check("all shots on disk", all(os.path.exists(p) for p in paths))
        check("shots differ",
              len({os.path.getsize(p) for p in paths}) > 1,
              "identical file sizes suggest the camera never moved")
        for p in paths:
            print(f"  {os.path.basename(p)}  {os.path.getsize(p)} bytes")

        print("\n[error handling]")
        try:
            c.call("no.such.method")
            check("unknown method rejected", False, "expected an error")
        except GlGenError as e:
            check("unknown method rejected", "unknown method" in str(e), str(e))

        print("\n[scene.clear]")
        cleared = c.call("scene.clear", {"name": "RpcBoulder"})
        check("removed what it spawned", cleared["destroyed"] == 1,
              str(cleared["destroyed"]))

    print()
    if failures:
        print(f"FAILED: {failures} check(s)")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

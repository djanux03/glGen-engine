#!/usr/bin/env python3
"""Golden-image and metric regression for glGen's asset pipeline.

Phase 6 of AI_ASSET_PIPELINE_PLAN.md. Renders every loaded recipe on a fixed
turntable and compares against stored references, plus the mesh metrics that
pin down a generator change exactly.

Two signals, because they fail differently:

  METRICS (triangle count, vertex count, bounds) catch "a generator changed"
  precisely and cheaply. The diff names the number that moved. They say nothing
  about shading.

  IMAGES catch what metrics cannot -- a shader regression, a material that
  stopped binding, a flipped normal. They only work because the renderer's
  animation clock can be pinned (VulkanRenderer::Params::fixedTimeSeconds);
  without that, drifting clouds alone would fail every comparison.

Usage:

    GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe &
    python Tools/glgen-regress/regress.py            # check
    python Tools/glgen-regress/regress.py --update   # accept current output

No third-party dependencies: the PNG reader uses stdlib zlib, which handles the
non-interlaced 8-bit files stb_image_write produces.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import zlib
from typing import Any, Dict, List, Tuple

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "glgen-client")
)
from glgen_client import GlGenClient, GlGenError  # noqa: E402

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
GOLDEN_DIR = os.path.join(REPO_ROOT, "Tests", "golden")
WORK_DIR = os.path.join(REPO_ROOT, "captures", "regress")
METRICS_FILE = os.path.join(GOLDEN_DIR, "metrics.json")

# Per-channel difference treated as noise. Renders on one machine are
# effectively deterministic once the clock is pinned, so this is deliberately
# tight -- a loose tolerance is how a regression check quietly stops working.
CHANNEL_TOLERANCE = 4
MAX_DIFFERING_FRACTION = 0.002
# Bounds are floats through a render pipeline; compare to millimetres.
BOUNDS_TOLERANCE = 1e-3


# --------------------------------------------------------------------------
# Minimal PNG reader (stdlib only)
# --------------------------------------------------------------------------


def read_png(path: str) -> Tuple[int, int, bytes]:
    """Decode a non-interlaced 8-bit RGB/RGBA PNG to (width, height, rgba)."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} is not a PNG")

    pos = 8
    width = height = 0
    channels = 4
    idat = bytearray()
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        chunk_type = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # length + type + data + crc

        if chunk_type == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB", chunk[:10])
            if depth != 8 or chunk[12] != 0 or color_type not in (2, 6):
                raise ValueError(
                    f"{path}: only 8-bit non-interlaced RGB/RGBA is supported")
            channels = 3 if color_type == 2 else 4
        elif chunk_type == b"IDAT":
            idat += chunk
        elif chunk_type == b"IEND":
            break

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    previous = bytearray(stride)
    offset = 0
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset:offset + stride])
        offset += stride

        # Undo the scanline filter: `a` is the byte to the left, `b` above,
        # `c` above-left.
        if filter_type == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:    # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif filter_type != 0:
            raise ValueError(f"{path}: unknown PNG filter {filter_type}")

        base = y * width * 4
        if channels == 4:
            out[base:base + stride] = line
        else:
            for x in range(width):
                out[base + x * 4:base + x * 4 + 3] = line[x * 3:x * 3 + 3]
                out[base + x * 4 + 3] = 255
        previous = line

    return width, height, bytes(out)


def compare_png(golden_path: str, actual_path: str) -> Dict[str, Any]:
    gw, gh, golden = read_png(golden_path)
    aw, ah, actual = read_png(actual_path)
    if (gw, gh) != (aw, ah):
        return {"ok": False, "reason": f"size {gw}x{gh} vs {aw}x{ah}"}

    pixels = gw * gh
    differing = 0
    worst = 0
    total = 0
    for i in range(0, len(golden), 4):
        delta = max(abs(golden[i] - actual[i]),
                    abs(golden[i + 1] - actual[i + 1]),
                    abs(golden[i + 2] - actual[i + 2]))
        total += delta
        if delta > worst:
            worst = delta
        if delta > CHANNEL_TOLERANCE:
            differing += 1

    fraction = differing / pixels if pixels else 0.0
    return {
        "ok": fraction <= MAX_DIFFERING_FRACTION,
        "meanDelta": total / pixels if pixels else 0.0,
        "reason": (f"{fraction * 100:.2f}% of pixels differ by more than "
                   f"{CHANNEL_TOLERANCE} (worst channel delta {worst})"),
    }


# --------------------------------------------------------------------------


def compare_metrics(recipe_id: str, before: Dict[str, Any],
                    after: Dict[str, Any]) -> List[str]:
    problems: List[str] = []
    for key in ("assetId", "triangles", "vertices", "submeshes"):
        if before.get(key) != after.get(key):
            problems.append(f"{recipe_id}: {key} {before.get(key)} -> "
                            f"{after.get(key)}")
    for key in ("boundsMin", "boundsMax"):
        old, new = before.get(key), after.get(key)
        if not old or not new or len(old) != len(new):
            problems.append(f"{recipe_id}: {key} missing or malformed")
            continue
        if any(abs(o - n) > BOUNDS_TOLERANCE for o, n in zip(old, new)):
            problems.append(f"{recipe_id}: {key} "
                            f"{[round(v, 4) for v in old]} -> "
                            f"{[round(v, 4) for v in new]}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description="glGen asset regression check.")
    parser.add_argument("--update", action="store_true",
                        help="Overwrite the goldens with the current output.")
    parser.add_argument("--update-images", action="store_true",
                        help="Accept reviewed shading/framing changes; keep geometry metrics gated.")
    parser.add_argument("--views", type=int, default=2,
                        help="Turntable views rendered per recipe.")
    parser.add_argument("--only", help="Only recipes whose id contains this.")
    parser.add_argument("--metrics-only", action="store_true",
                        help="Skip rendering; compare mesh metrics only. Fast, "
                             "and does not need a visible window.")
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("GLGEN_AGENT_PORT", "8787")))
    parser.add_argument("--report", help="Write metrics, image results and capture hashes as JSON.")
    args = parser.parse_args()

    os.makedirs(GOLDEN_DIR, exist_ok=True)
    os.makedirs(WORK_DIR, exist_ok=True)

    try:
        client = GlGenClient(port=args.port)
    except OSError:
        print(f"error: nothing is listening on 127.0.0.1:{args.port}.\n"
              f"Start the engine with GLGEN_AGENT_PORT={args.port} set.",
              file=sys.stderr)
        return 1

    baseline: Dict[str, Any] = {}
    if os.path.exists(METRICS_FILE):
        with open(METRICS_FILE, "r", encoding="utf-8") as handle:
            baseline = json.load(handle)

    failures: List[str] = []
    metrics: Dict[str, Any] = {}
    review: Dict[str, Any] = {"recipes": {}, "failures": []}

    with client:
        recipe_ids = client.eval("assets.list()") or []
        if args.only:
            recipe_ids = [r for r in recipe_ids if args.only in r]
        if not recipe_ids:
            print("error: the engine has no recipes loaded", file=sys.stderr)
            return 1

        mode = "updating goldens" if args.update or args.update_images else "checking"
        print(f"{mode}: {len(recipe_ids)} recipes\n")

        for recipe_id in recipe_ids:
            print(recipe_id)
            asset_id = client.eval(f"assets.asset_id({json.dumps(recipe_id)})")
            if not asset_id:
                failures.append(f"{recipe_id}: no asset id")
                print("  FAIL  no asset id")
                continue

            info = client.eval(f"assets.info({json.dumps(asset_id)})")
            if not info:
                failures.append(f"{recipe_id}: no mesh registered")
                print("  FAIL  no mesh registered")
                continue

            entry = {
                "assetId": asset_id,
                "triangles": info["triangles"],
                "vertices": info["vertices"],
                "submeshes": info["submeshes"],
                "boundsMin": [round(v, 5) for v in info["boundsMin"]],
                "boundsMax": [round(v, 5) for v in info["boundsMax"]],
            }
            metrics[recipe_id] = entry
            record = {"metrics": entry, "metricFailures": [], "images": []}
            review["recipes"][recipe_id] = record
            print(f"  {entry['triangles']} tris, {entry['vertices']} verts, "
                  f"{entry['submeshes']} submesh(es)")

            if not args.update and recipe_id in baseline:
                problems = compare_metrics(recipe_id, baseline[recipe_id], entry)
                record["metricFailures"] = problems
                for problem in problems:
                    failures.append(problem)
                    print(f"  FAIL  {problem}")
                if not problems:
                    print("  ok    metrics")
            elif not args.update:
                print("  note  new recipe, no baseline")

            if args.metrics_only:
                continue

            slug = recipe_id.replace("/", "_")
            base = os.path.join(WORK_DIR, slug)
            try:
                paths = client.turntable(asset_id, base, steps=args.views)
            except GlGenError as exc:
                failures.append(f"{recipe_id}: render failed: {exc}")
                print(f"  FAIL  render: {exc}")
                continue

            for index, produced in enumerate(paths):
                name = f"{slug}_{index:02d}.png"
                golden = os.path.join(GOLDEN_DIR, name)
                with open(produced, "rb") as capture:
                    capture_hash = hashlib.sha256(capture.read()).hexdigest()
                image_record = {"name": name, "actual": produced,
                    "sha256": capture_hash}
                record["images"].append(image_record)
                if args.update or args.update_images:
                    shutil.copyfile(produced, golden)
                    print(f"  saved {name}")
                    image_record["accepted"] = True
                    continue
                if not os.path.exists(golden):
                    failures.append(f"{recipe_id} {name}: missing golden")
                    print(f"  FAIL  {name}: missing golden (use --update after review)")
                    image_record["comparison"] = {"ok": False, "reason": "missing golden"}
                    continue
                try:
                    result = compare_png(golden, produced)
                    image_record["comparison"] = result
                except ValueError as exc:
                    failures.append(f"{recipe_id} {name}: {exc}")
                    print(f"  FAIL  {name}: {exc}")
                    continue
                if result["ok"]:
                    print(f"  ok    {name}  (mean delta "
                          f"{result['meanDelta']:.2f})")
                else:
                    failures.append(f"{recipe_id} {name}: {result['reason']}")
                    print(f"  FAIL  {name}  {result['reason']}")

    if args.update or not baseline:
        # A subset update must not erase references for all other recipes.
        if args.only:
            metrics = {**baseline, **metrics}
        with open(METRICS_FILE, "w", encoding="utf-8") as handle:
            json.dump(metrics, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print(f"\nwrote {os.path.relpath(METRICS_FILE, REPO_ROOT)}")
    elif not args.only:
        # Only meaningful for a full run: with --only, everything else is
        # missing by request and saying so is just noise.
        for recipe_id in baseline:
            if recipe_id not in metrics:
                print(f"note: '{recipe_id}' is in the baseline but was not "
                      f"rendered (deleted recipe?)")

    if args.report:
        review["failures"] = failures
        with open(args.report, "w", encoding="utf-8") as handle:
            json.dump(review, handle, indent=2, sort_keys=True)
            handle.write("\n")
    print()
    if failures:
        print(f"FAILED: {len(failures)} difference(s)")
        for failure in failures:
            print(f"  - {failure}")
        print("\nIf these changes are intended, re-run with --update.")
        return 1
    print("no regressions")
    return 0


if __name__ == "__main__":
    sys.exit(main())

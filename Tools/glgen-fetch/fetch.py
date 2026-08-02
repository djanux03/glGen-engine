#!/usr/bin/env python3
"""glgen-fetch — bring an external 3D model into glGen as a recipe.

Phase 5 of AI_ASSET_PIPELINE_PLAN.md: the landing point for Option A
(text-to-3D services, asset stores, hand-modelled files). Two things happen
here, and the second is the one that matters:

  1. OBTAIN the file -- either already on disk (--from-file) or downloaded
     from a text-to-3D provider (--prompt, needs an API key).
  2. CONDITION it, so it is usable in this engine rather than merely present:
     normalize scale to metres, fix the up-axis, put the pivot where placement
     code expects it, and DECIMATE to a class-appropriate triangle budget.

Step 2 runs inside the engine over the command port, because the engine
already has the .obj/.gltf/.glb/.fbx parsers and the decimator, and because
that way the conditioned result is verified by the thing that will render it.

The output is a recipe in assets/recipes/, not a baked mesh. An imported asset
is then the same kind of thing as a generated one -- scatter layers, the asset
library and the MCP tools do not care which it is.

    # condition a local file (fully supported)
    python Tools/glgen-fetch/fetch.py --from-file "assets/wrench.glb" \
        --slug wrench --class prop --render

    # fetch from a service (requires your own API key -- see PROVIDERS)
    python Tools/glgen-fetch/fetch.py --prompt "a mossy boulder" \
        --provider meshy --class rock

Run the engine with its command port open first:

    GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sys
from typing import Any, Dict, Optional

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "glgen-client")
)
from glgen_client import GlGenClient, GlGenError  # noqa: E402

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
IMPORT_DIR = os.path.join(REPO_ROOT, "assets", "imported")
RECIPE_DIR = os.path.join(REPO_ROOT, "assets", "recipes")

SUPPORTED_EXTENSIONS = {".obj", ".gltf", ".glb", ".fbx"}


# --------------------------------------------------------------------------
# Asset classes
# --------------------------------------------------------------------------
#
# The budgets are the same ones the procedural generators are held to (plan
# §3.3): this renderer builds one acceleration structure per unique mesh, and
# scattered content multiplies every triangle by its instance count. An
# unconditioned 80k-triangle download is not a smaller version of a good asset,
# it is an unusable one.

# Upright classes normalize HEIGHT; classes whose orientation is unknown
# normalize the LONGEST axis instead. Normalizing the height of something lying
# flat scales its thin axis -- a wrench 0.09 units tall and 0.7 long becomes
# 1 m tall and 8 m long, which is how that mistake actually presents.
CLASSES: Dict[str, Dict[str, Any]] = {
    "tree": {
        "maxTriangles": 4000,
        "normalizeHeight": 8.0,
        "recenter": "baseY",
        "note": "scattered by the hundred; pivot at the base for terrain placement",
    },
    "rock": {
        "maxTriangles": 1500,
        "normalizeSize": 1.2,
        "recenter": "baseY",
        "note": "scattered; sits on the ground, no meaningful up axis",
    },
    "grass": {
        "maxTriangles": 200,
        "normalizeHeight": 0.35,
        "recenter": "baseY",
        "note": "instanced by the ten-thousand; the tightest budget there is",
    },
    "prop": {
        "maxTriangles": 8000,
        "normalizeSize": 1.0,
        "recenter": "baseY",
        "note": "placed by hand; orientation unknown, so the longest axis sets scale",
    },
    "hero": {
        "maxTriangles": 20000,
        "normalizeSize": 2.0,
        "recenter": "baseY",
        "note": "few instances, seen close up",
    },
    "raw": {
        "maxTriangles": 0,
        "recenter": "none",
        "note": "no conditioning at all; you are on your own",
    },
}


# --------------------------------------------------------------------------
# Providers
# --------------------------------------------------------------------------
#
# A provider turns a text prompt into a downloaded model file. The contract is
# one function:
#
#     fetch(prompt: str, out_path: str, api_key: str) -> str   # path written
#
# NONE OF THESE HAVE BEEN EXERCISED. Text-to-3D APIs need a paid account, and
# nothing here has been run against a live service -- treat an adapter as a
# starting point to verify against your provider's current documentation, not
# as known-good code. The conditioning half of this tool (--from-file) is what
# has actually been tested.
#
# Every provider follows the same three-step shape: POST a job, poll until it
# reports done, GET the result file. If you only ever use one service, deleting
# the rest of this section costs nothing.


def _http_json(url: str, payload: Optional[dict], headers: Dict[str, str],
               method: str = "POST") -> dict:
    import urllib.request

    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.loads(response.read().decode("utf-8"))


def _download(url: str, out_path: str) -> str:
    import urllib.request

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with urllib.request.urlopen(url, timeout=600) as response, open(out_path, "wb") as f:
        shutil.copyfileobj(response, f)
    return out_path


def fetch_generic(prompt: str, out_path: str, api_key: str, *,
                  submit_url: str, status_url: str,
                  poll_seconds: float = 5.0, timeout_seconds: float = 900.0) -> str:
    """Reference submit/poll/download flow. VERIFY AGAINST YOUR PROVIDER'S DOCS.

    The field names below (`result`, `status`, `model_urls.glb`) are the shape
    several services use, but they are NOT a standard and yours will differ.
    """
    import time

    headers = {"Authorization": f"Bearer {api_key}",
               "Content-Type": "application/json"}
    job = _http_json(submit_url, {"mode": "preview", "prompt": prompt}, headers)
    job_id = job.get("result") or job.get("id")
    if not job_id:
        raise RuntimeError(f"provider did not return a job id: {job}")

    print(f"  job {job_id}; polling (text-to-3D typically takes 30 s - 3 min)")
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        time.sleep(poll_seconds)
        status = _http_json(f"{status_url}/{job_id}", None, headers, method="GET")
        state = str(status.get("status", "")).upper()
        if state in ("SUCCEEDED", "COMPLETED", "SUCCESS"):
            urls = status.get("model_urls") or {}
            url = urls.get("glb") or status.get("model_url")
            if not url:
                raise RuntimeError(f"job finished without a model url: {status}")
            print(f"  downloading {url[:70]}...")
            return _download(url, out_path)
        if state in ("FAILED", "ERROR", "EXPIRED"):
            raise RuntimeError(f"provider job failed: {status}")
        print(f"  ... {state or 'pending'}")
    raise RuntimeError("provider job timed out")


PROVIDERS = {
    # name: (submit endpoint, status endpoint, env var holding the key)
    "meshy": ("https://api.meshy.ai/openapi/v2/text-to-3d",
              "https://api.meshy.ai/openapi/v2/text-to-3d", "MESHY_API_KEY"),
    "tripo": ("https://api.tripo3d.ai/v2/openapi/task",
              "https://api.tripo3d.ai/v2/openapi/task", "TRIPO_API_KEY"),
}


def fetch_from_provider(provider: str, prompt: str, out_path: str) -> str:
    if provider not in PROVIDERS:
        raise RuntimeError(
            f"unknown provider '{provider}'. Known: {', '.join(PROVIDERS)}. "
            f"Adding one means writing a submit/poll/download function -- see "
            f"fetch_generic."
        )
    submit_url, status_url, env_var = PROVIDERS[provider]
    api_key = os.environ.get(env_var, "")
    if not api_key:
        raise RuntimeError(
            f"{env_var} is not set. Text-to-3D services need a paid account; "
            f"get a key from the provider and export it. If you already have a "
            f"model file, use --from-file instead -- that is the path this "
            f"tool actually verifies."
        )
    print(f"  NOTE: the '{provider}' adapter has not been run against the live "
          f"service. If it fails, check the current API docs and adjust "
          f"fetch_generic().")
    return fetch_generic(prompt, out_path, api_key,
                         submit_url=submit_url, status_url=status_url)


# --------------------------------------------------------------------------
# Conditioning
# --------------------------------------------------------------------------


def slugify(text: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")
    return slug[:48] or "asset"


def condition(client: GlGenClient, rel_path: str, slug: str, asset_class: str,
              rotate: Optional[list] = None,
              overrides: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Import through the engine and report what conditioning did."""
    settings = dict(CLASSES[asset_class])
    settings.pop("note", None)
    if rotate:
        settings["rotateDeg"] = rotate
    if overrides:
        settings.update(overrides)

    recipe = {
        "id": f"imported/{slug}",
        "generator": "import.gltf",
        "seed": 0,
        "params": {"path": rel_path, **settings},
        "provenance": {"source": rel_path, "class": asset_class,
                       "tool": "glgen-fetch"},
    }
    result = client.call("assets.define", recipe)
    return {"recipe": recipe, **result}


def write_recipe(recipe: Dict[str, Any], slug: str) -> str:
    os.makedirs(RECIPE_DIR, exist_ok=True)
    path = os.path.join(RECIPE_DIR, f"imported_{slug}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(recipe, f, indent=2)
        f.write("\n")
    return path


# --------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Import and condition an external model as a glGen recipe.")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--from-file", help="Path to a model already on disk.")
    source.add_argument("--prompt", help="Text prompt for a text-to-3D service.")
    parser.add_argument("--provider", default="meshy",
                        help=f"Service for --prompt. Known: {', '.join(PROVIDERS)}")
    parser.add_argument("--slug", help="Short name. Defaults from the file or prompt.")
    parser.add_argument("--class", dest="asset_class", default="prop",
                        choices=sorted(CLASSES),
                        help="Conditioning preset; sets the triangle budget and scale.")
    parser.add_argument("--max-triangles", type=int,
                        help="Override the class triangle budget.")
    parser.add_argument("--height", type=float,
                        help="Override the target HEIGHT in metres (upright objects).")
    parser.add_argument("--size", type=float,
                        help="Override the target LONGEST DIMENSION in metres. "
                             "Safer when the orientation is unknown.")
    parser.add_argument("--rotate", help="Corrective rotation 'x,y,z' in degrees.")
    parser.add_argument("--render", action="store_true",
                        help="Render a turntable afterwards so you can check it.")
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("GLGEN_AGENT_PORT", "8787")))
    args = parser.parse_args()

    # -- obtain -------------------------------------------------------------
    if args.from_file:
        src = os.path.abspath(args.from_file)
        if not os.path.exists(src):
            print(f"error: no such file: {src}", file=sys.stderr)
            return 1
        slug = args.slug or slugify(os.path.splitext(os.path.basename(src))[0])
        ext = os.path.splitext(src)[1].lower()
        if ext not in SUPPORTED_EXTENSIONS:
            print(f"error: {ext} is not one of "
                  f"{', '.join(sorted(SUPPORTED_EXTENSIONS))}", file=sys.stderr)
            return 1
        os.makedirs(IMPORT_DIR, exist_ok=True)
        dst = os.path.join(IMPORT_DIR, slug + ext)
        # Copy rather than reference in place: the recipe should keep working
        # if the original is moved, and assets/imported is the reviewable home
        # for anything that came from outside.
        if os.path.abspath(src) != os.path.abspath(dst):
            shutil.copy2(src, dst)
        print(f"source: {src}")
        print(f"  -> {os.path.relpath(dst, REPO_ROOT)} "
              f"({os.path.getsize(dst) / 1024:.0f} KB)")
    else:
        slug = args.slug or slugify(args.prompt)
        dst = os.path.join(IMPORT_DIR, slug + ".glb")
        print(f"fetching '{args.prompt}' from {args.provider} ...")
        try:
            fetch_from_provider(args.provider, args.prompt, dst)
        except Exception as exc:  # noqa: BLE001
            print(f"error: {exc}", file=sys.stderr)
            return 1

    rel_path = os.path.relpath(dst, REPO_ROOT).replace("\\", "/")

    # -- condition ----------------------------------------------------------
    overrides: Dict[str, Any] = {}
    if args.max_triangles is not None:
        overrides["maxTriangles"] = args.max_triangles
    if args.height is not None:
        overrides["normalizeHeight"] = args.height
        overrides["normalizeSize"] = 0.0  # explicit height wins
    if args.size is not None:
        overrides["normalizeSize"] = args.size
    rotate = [float(v) for v in args.rotate.split(",")] if args.rotate else None

    preset = CLASSES[args.asset_class]
    merged = {**preset, **overrides}
    scale_note = (
        f"longest axis {merged['normalizeSize']} m"
        if merged.get("normalizeSize")
        else (f"height {merged['normalizeHeight']} m"
              if merged.get("normalizeHeight") else "original scale")
    )
    print(f"\nconditioning as '{args.asset_class}' ({preset['note']})")
    print(f"  budget {merged.get('maxTriangles', 0)} tris, {scale_note}")

    try:
        client = GlGenClient(port=args.port)
    except OSError:
        print(f"\nerror: nothing is listening on 127.0.0.1:{args.port}.\n"
              f"Start the engine with GLGEN_AGENT_PORT={args.port} set.",
              file=sys.stderr)
        return 1

    with client:
        try:
            result = condition(client, rel_path, slug, args.asset_class,
                               rotate, overrides)
        except GlGenError as exc:
            print(f"\nerror: the engine rejected the import: {exc}", file=sys.stderr)
            return 1

        print(f"\n  {result['assetId']}")
        print(f"  {result['triangles']} triangles after conditioning")
        for warning in result.get("warnings", []):
            print(f"  - {warning}")

        recipe_path = write_recipe(result["recipe"], slug)
        print(f"\nwrote {os.path.relpath(recipe_path, REPO_ROOT)}")
        print("  This asset now behaves exactly like a generated one: it "
              "reloads on edit and can be used in a scatter layer.")

        if args.render:
            base = os.path.join(REPO_ROOT, "captures", "imported", slug)
            print("\nrendering ...")
            paths = client.turntable(result["assetId"], base, steps=3)
            for p in paths:
                print(f"  {os.path.relpath(p, REPO_ROOT)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""MCP server for glGen — Phase 4 of AI_ASSET_PIPELINE_PLAN.md.

Bridges an MCP client (Claude, or any other) to the engine's command port. The
point is the closed loop: generate an asset from parameters, RENDER it, look at
the image, adjust, repeat. Without the image half this is just a remote control.

Two design notes worth reading before editing:

* NO SDK DEPENDENCY. MCP's stdio transport is newline-delimited JSON-RPC 2.0,
  the same protocol the engine's own port speaks, and the surface needed here
  is small (initialize / tools/list / tools/call). Writing it against the
  standard library keeps this runnable with a bare `python server.py` and no
  install step. If the surface grows (resources, prompts, sampling), switch to
  the official `mcp` package rather than growing this file.

* TOOLS ARE GENERATED FROM THE ENGINE'S SCHEMAS. On connect, the server asks
  the engine for every generator and emits one `glgen_create_<generator>` tool
  whose inputSchema IS that generator's parameter schema -- real ranges, real
  defaults, real descriptions. This is the payoff of the schema doing double
  duty (plan §2, point 4): the model sees that tree.v1's height is 0.3..40 m
  before it calls anything, instead of guessing and being corrected.

Usage (as an MCP server, from a client's config):

    command: python
    args:    ["<repo>/Tools/glgen-mcp/server.py"]
    env:     {"GLGEN_AGENT_PORT": "8787", "GLGEN_EXE": "<repo>/Build-vs18/bin/Release/glGenVk.exe"}

Self-test against a running engine, no MCP client needed:

    python Tools/glgen-mcp/server.py --selftest
"""

from __future__ import annotations

import base64
import copy
import json
import os
import subprocess
import sys
import time
import uuid
from typing import Any, Dict, List, Optional

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "glgen-client")
)
from glgen_client import GlGenClient, GlGenError  # noqa: E402

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "glgen"
SERVER_VERSION = "0.1.0"

# Sent to the client on initialize. This is the only place the server can
# explain HOW to work with it -- tool descriptions cover one call each and
# cannot say "render before you believe it" or "reuse the id to iterate".
# Everything here is load-bearing; it goes into the model's context on every
# session, so anything that does not change behaviour has been left out.
SERVER_INSTRUCTIONS = """\
glGen is a 3D game engine. You author assets here by describing them as
PARAMETERS, not by writing geometry: each glgen_create_* tool's schema is the
engine's own validation schema, with real ranges, defaults and enums.

THE LOOP. Creating an asset is half the job. Always:

  1. glgen_create_<generator>  with an `id` like "tree/windswept_oak"
  2. glgen_render_asset        and LOOK at the images
  3. adjust the parameters, REUSING THE SAME id, and render again

Reusing an id regenerates that asset in place, so anything already placed in
the scene updates too. That is what makes iteration cheap. A new id makes a
new asset.

Do not describe an asset as finished until you have rendered it. Parameters
that read sensibly often look wrong -- branches at a wide angle become a
clothes-line, a sparse canopy reads as a dying tree -- and the render is the
only way you will know.

WHEN YOU JUDGE A RENDER, in this order:
  silhouette   is the shape readable? this survives at distance; nothing else does
  proportion   is it the size a real one would be? metres, and Y is up
  detail       colour and texture come last and matter least

TWO FEEDBACK CHANNELS, both worth reading. The images show proportion and
silhouette. The TEXT of a create call reports what an image cannot: parameters
that were clamped to their valid range, triangle budgets exceeded, material
slots that matched nothing. A clamped parameter means the asset is not what you
asked for. Act on those messages rather than re-rendering and guessing.

TRIANGLE BUDGETS ARE REAL. This renderer builds one ray-tracing acceleration
structure per unique mesh, and scattered content multiplies every triangle by
its instance count. Going over budget is a warning, not a rejection, but for
anything scattered it is a genuine performance problem. Get variety from
DIFFERENT SEEDS on a few recipes -- one recipe with six seeds is six distinct
individuals for the cost of six small meshes -- not from a unique mesh per
placement.

CONVENTIONS. Units are metres. Y is up. An asset's pivot sits at its base, so
placing it at a terrain height puts it on the ground; glgen_place_asset with
onGround does that for you.

BEYOND THE TOOLS. glgen_eval_lua runs Lua inside the engine and returns the
result. Use it for anything not covered: terrain.regenerate{...},
assets.list(), assets.info(assetId), scene.stats(), entity manipulation.
Prefer a typed tool when one fits.

If a call fails, the error text is specific -- read it before retrying. An
unknown parameter is reported by name; a bad enum lists the valid values.\
"""

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
DEFAULT_PORT = int(os.environ.get("GLGEN_AGENT_PORT", "8787"))
DEFAULT_EXE = os.environ.get(
    "GLGEN_EXE", os.path.join(REPO_ROOT, "Build-vs18", "bin", "Release", "glGenVk.exe")
)
CAPTURE_DIR = os.path.join(REPO_ROOT, "captures", "mcp")
CHARACTER_JOB_DIR = os.path.join(REPO_ROOT, ".glgen", "jobs")

# MCP is a stateful authoring session.  Keeping only the recipes made through
# this session is intentional: we can offer safe intent-level edits without
# guessing how an arbitrary pre-existing runtime asset was authored.
SESSION_RECIPES: Dict[str, Dict[str, Any]] = {}


def log(message: str) -> None:
    """Diagnostics go to stderr: stdout is the MCP transport."""
    print(f"[glgen-mcp] {message}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# Engine connection
# --------------------------------------------------------------------------


class Engine:
    """Lazily connects, and launches glGenVk if nothing is listening."""

    def __init__(self, port: int = DEFAULT_PORT, exe: str = DEFAULT_EXE) -> None:
        self.port = port
        self.exe = exe
        self._client: Optional[GlGenClient] = None
        self._process: Optional[subprocess.Popen] = None

    def client(self) -> GlGenClient:
        if self._client is not None:
            return self._client
        try:
            self._client = GlGenClient(port=self.port)
            log(f"connected to a running engine on port {self.port}")
            return self._client
        except OSError:
            pass

        if not os.path.exists(self.exe):
            raise GlGenError(
                f"nothing is listening on 127.0.0.1:{self.port} and no engine "
                f"binary at {self.exe}. Start glGen with GLGEN_AGENT_PORT set, "
                f"or point GLGEN_EXE at the executable."
            )

        log(f"launching {self.exe}")
        env = dict(os.environ, GLGEN_AGENT_PORT=str(self.port))
        self._process = subprocess.Popen(
            [self.exe], cwd=REPO_ROOT, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        # Vulkan init plus terrain generation takes a few seconds on a cold
        # start; retry rather than failing the client's first tool call.
        for _ in range(60):
            time.sleep(0.5)
            try:
                self._client = GlGenClient(port=self.port)
                log("engine is up")
                return self._client
            except OSError:
                if self._process.poll() is not None:
                    raise GlGenError("the engine exited during startup")
        raise GlGenError("the engine did not open its command port in time")

    def call(self, method: str, params: Optional[Dict[str, Any]] = None) -> Any:
        try:
            return self.client().call(method, params)
        except (OSError, GlGenError) as exc:
            # A dropped socket should not poison every later call.
            if isinstance(exc, OSError):
                self._client = None
            raise

    def close(self) -> None:
        if self._client:
            self._client.close()
            self._client = None


# --------------------------------------------------------------------------
# Tool definitions
# --------------------------------------------------------------------------


def sanitize(name: str) -> str:
    return name.replace(".", "_").replace("-", "_")


STATIC_TOOLS: List[Dict[str, Any]] = [
    {
        "name": "glgen_info",
        "description": (
            "Current state of the glGen engine: frame count, available "
            "generators, and the recipes already loaded. Call this first to "
            "confirm the engine is reachable."
        ),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "glgen_render_asset",
        "description": (
            "Render a generated asset and RETURN THE IMAGES so you can judge "
            "it. Orbits the asset against open sky with fixed lighting, which "
            "makes two renders directly comparable. This is how you check "
            "whether an asset actually looks right -- always render after "
            "creating something, then adjust parameters and render again. "
            "Judge silhouette first, then proportion, then colour."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "assetId": {
                    "type": "string",
                    "description": "Asset id returned by a glgen_create_* tool.",
                },
                "views": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 8,
                    "default": 3,
                    "description": "How many viewpoints around the asset.",
                },
            },
            "required": ["assetId"],
        },
    },
    {
        "name": "glgen_screenshot",
        "description": (
            "Capture the engine's current view and return it as an image. Use "
            "after placing assets to see them in context, in the real scene "
            "with terrain and lighting."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "includeUi": {
                    "type": "boolean",
                    "default": False,
                    "description": "Include the editor panels in the shot.",
                }
            },
        },
    },
    {
        "name": "glgen_place_asset",
        "description": (
            "Place an asset in the world at a position, optionally dropped "
            "onto the terrain surface."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "assetId": {"type": "string"},
                "pos": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": "World position [x, y, z] in metres.",
                },
                "rot": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                    "description": "Euler rotation in degrees.",
                },
                "scale": {"type": "number", "default": 1.0},
                "name": {"type": "string"},
                "onGround": {
                    "type": "boolean",
                    "default": True,
                    "description": "Snap Y to the procedural terrain height.",
                },
            },
            "required": ["assetId"],
        },
    },
    {
        "name": "glgen_scene_query",
        "description": "List the mesh entities currently in the scene.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "Only entities whose name contains this.",
                }
            },
        },
    },
    {
        "name": "glgen_scene_clear",
        "description": (
            "Remove spawned entities. With no filter this clears every mesh "
            "entity except the player, so pass a name filter unless you mean "
            "to wipe the scene."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"name": {"type": "string"}},
        },
    },
    {
        "name": "glgen_set_render_params",
        "description": (
            "Adjust camera, sun and post-processing. Useful for framing a "
            "shot or changing time of day before a screenshot."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "camPos": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 3,
                    "maxItems": 3,
                },
                "camYaw": {"type": "number"},
                "camPitch": {"type": "number"},
                "sunPitch": {
                    "type": "number",
                    "description": "Sun elevation. Negative is night.",
                },
                "sunYaw": {"type": "number"},
                "exposure": {"type": "number"},
                "autoExposure": {"type": "boolean"},
                "autoExposureSpeed": {"type": "number", "minimum": 0, "maximum": 1},
                "autoExposureMin": {"type": "number", "minimum": 0},
                "autoExposureMax": {"type": "number", "minimum": 0},
                **{key: {"type": "number"} for key in (
                    "cloudCoverage", "cloudSoftness", "cloudDeckHeight", "cloudFeatureScale",
                    "cloudOpticalDensity", "cloudSunOcclusion", "cloudStrength",
                    "cloudLayerThickness", "cloudDensityMultiplier", "cloudShapeScale",
                    "cloudDetailScale", "cloudWeatherScale", "cloudLightAbsorption",
                    "cloudAmbientStrength", "cloudCurlStrength", "cloudPhaseG",
                    "cloudSilverIntensity", "cloudSilverSpread", "cloudPowderStrength",
                    "cloudMaxMarchDist", "cloudMaxSteps", "cloudLightTaps",
                    "cloudTypeBias", "cloudDetailStrength")},
                "paintedClouds": {"type": "boolean"},
                "cloudVolumetricEnabled": {"type": "boolean"},
                "fogDensity": {"type": "number"},
                "atmosphere": {
                    "type": "object",
                    "description": "Schema v2 shared fog medium. Distances are metres, extinction is m^-1. Explicit nested values override legacy flat aliases.",
                    "properties": {
                        "version": {"type": "integer", "enum": [2]},
                        "quality": {"enum": [0, 1, "balanced", "high"]},
                        **{key: {"type": "boolean"} for key in (
                            "enabled", "physicalSky", "cloudShadows", "cloudHistory", "valleyPooling", "histogramExposure", "history", "directionalShadows", "skyVisibility",
                            "terrainMist", "bloomFireflySuppression")},
                        "range": {"type": "number", "minimum": 1, "maximum": 2000},
                        "start": {"type": "number", "minimum": 0},
                        "groundReference": {"type": "number"},
                        "historyWeight": {"type": "number", "minimum": 0, "maximum": 0.9},
                        "valleyExtinction": {"type": "number", "minimum": 0, "maximum": 10},
                        "valleyDepth": {"type": "number", "minimum": 0, "maximum": 64},
                        "exposureKey": {"type": "number", "minimum": 0.001, "maximum": 1},
                        "exposureMin": {"type": "number", "minimum": 0.000001, "maximum": 10000},
                        "exposureMax": {"type": "number", "minimum": 0.000001, "maximum": 10000},
                        **{key: {"type": "number", "minimum": 0, "maximum": 10} for key in (
                            "groundExtinction", "groundFalloff", "dustExtinction",
                            "terrainExtinction", "terrainFalloff")},
                        "waterBoost": {"type": "number", "minimum": 0, "maximum": 20},
                        **{key: {"type": "number", "minimum": -0.95, "maximum": 0.95} for key in (
                            "groundAnisotropy", "dustAnisotropy")},
                        **{key: {"type": "array", "minItems": 3, "maxItems": 3,
                            "items": {"type": "number", "minimum": 0, "maximum": 1}}
                            for key in ("groundAlbedo", "dustAlbedo")},
                        "groundTintStrength": {"type": "number", "minimum": 0, "maximum": 1},
                        "bloomStrength": {"type": "number", "minimum": 0, "maximum": 1},
                    },
                },
                "pointLights": {
                    "type": "array",
                    "maxItems": 4,
                    "description": "Local practical lights for lamps and fixtures.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                            "color": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                            "radius": {"type": "number", "minimum": 0.1},
                            "intensity": {"type": "number", "minimum": 0},
                            "volumetricParticipation": {"type": "number", "minimum": 0, "maximum": 1},
                            "volumetricShadows": {"type": "boolean"},
                        },
                        "required": ["position", "color", "radius", "intensity"],
                    },
                },
            },
        },
    },
    {
        "name": "glgen_eval_lua",
        "description": (
            "Run Lua in the engine and return the result. The escape hatch "
            "for anything the other tools do not cover: terrain.regenerate{}, "
            "assets.list(), scene.stats(), entity manipulation. Prefer the "
            "typed tools when one fits."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {
                    "type": "string",
                    "description": "A Lua expression or statement block.",
                }
            },
            "required": ["code"],
        },
    },
]

# These tools compose the schema-generated creation tools into an editing
# workflow. Geometry remains engine-owned; the server merely retains the
# small JSON recipes needed to make an in-place revision meaningful.
STATIC_TOOLS.extend([
    {
        "name": "glgen_modify_asset",
        "description": "Patch a session-created asset's generator parameters, seed, or material recipes, regenerate it in place, and keep all existing placements linked to the revised asset.",
        "inputSchema": {"type": "object", "properties": {
            "assetId": {"type": "string"}, "params": {"type": "object", "description": "Only the generator parameters to replace."},
            "seed": {"type": "integer"}, "material": {"type": "object", "description": "Material-slot recipes to replace."},
        }, "required": ["assetId"]},
    },
    {
        "name": "glgen_apply_material",
        "description": "Apply or replace procedural texture recipes on a session-created asset's named material slots, then regenerate it in place. Texture generator names and schemas are reported by glgen_info.",
        "inputSchema": {"type": "object", "properties": {
            "assetId": {"type": "string"}, "material": {"type": "object", "description": "Slot map, e.g. {sculpt:{generator:'tex.rock',params:{...}}}."},
        }, "required": ["assetId", "material"]},
    },
    {
        "name": "glgen_create_assembly",
        "description": "Compose existing generated assets into a named multi-part scene assembly. Use it for a sculpted body plus sweep horns, a revolved lamp plus panel housing, or any reusable hero prop made from coherent parts.",
        "inputSchema": {"type": "object", "properties": {
            "name": {"type": "string", "description": "Shared name prefix for the placed parts."},
            "pos": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3, "default": [0, 0, 0]},
            "parts": {"type": "array", "minItems": 1, "maxItems": 32, "items": {"type": "object", "properties": {
                "assetId": {"type": "string"}, "pos": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3},
                "rot": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}, "scale": {"type": "number"}, "name": {"type": "string"},
            }, "required": ["assetId"]}},
        }, "required": ["name", "parts"]},
    },
    {
        "name": "glgen_render_compare",
        "description": "Render two assets from identical bounds-framed viewpoints and return a contact-sheet sequence plus their triangle and bounds metrics. Use to judge whether an iteration actually improved the result.",
        "inputSchema": {"type": "object", "properties": {
            "beforeAssetId": {"type": "string"}, "afterAssetId": {"type": "string"}, "views": {"type": "integer", "minimum": 1, "maximum": 4, "default": 2},
        }, "required": ["beforeAssetId", "afterAssetId"]},
    },
    {
        "name": "glgen_validate_asset",
        "description": "Return an asset's triangle count, vertex count, submesh count and physical bounds, with concise authoring guidance based on those facts.",
        "inputSchema": {"type": "object", "properties": {"assetId": {"type": "string"}}, "required": ["assetId"]},
    },
    {
        "name": "glgen_create_lod_set",
        "description": "Create lower-detail recipe variants for a session-created asset by lowering its 'detail' parameter. Best for generators that expose detail (sculpt, rock); returns the generated LOD asset IDs and their metrics.",
        "inputSchema": {"type": "object", "properties": {
            "assetId": {"type": "string"}, "levels": {"type": "integer", "minimum": 1, "maximum": 3, "default": 2},
        }, "required": ["assetId"]},
    },
    {
        "name": "glgen_import_reference",
        "description": "Import a local GLB/glTF/OBJ/FBX reference mesh through the engine's conditioned import.gltf recipe. It normalizes and decimates the result so it obeys glGen's ray-tracing budget.",
        "inputSchema": {"type": "object", "properties": {
            "id": {"type": "string"}, "path": {"type": "string", "description": "Local mesh path accessible to the glGen process."},
            "maxTriangles": {"type": "integer", "minimum": 100, "maximum": 50000, "default": 8000},
            "normalizeSize": {"type": "number", "minimum": 0.01, "maximum": 100, "default": 1.0},
        }, "required": ["id", "path"]},
    },
])

STATIC_TOOLS.extend([
    {"name": "glgen_character_begin", "description": "Start a persistent text-to-character job. Creates a deterministic stylized static humanoid base; use character_edit, character_review, and character_finalize to complete it.", "inputSchema": {"type": "object", "properties": {"brief": {"type": "string"}, "name": {"type": "string"}, "targetTriangles": {"type": "integer", "minimum": 1000, "maximum": 20000, "default": 15000}, "seed": {"type": "integer"}}, "required": ["brief", "name"]}},
    {"name": "glgen_character_edit", "description": "Apply one semantic edit to a persistent character job. Allowed operations are reshape, add_garment, add_feature, add_hair, add_accessory, assign_material, mirror, smooth, remesh, and simplify. At most eight edits are allowed before finalization.", "inputSchema": {"type": "object", "properties": {"jobId": {"type": "string"}, "expectedVersion": {"type": "integer"}, "operation": {"type": "object", "description": "Semantic operation object, always including string field 'op'."}}, "required": ["jobId", "expectedVersion", "operation"]}},
    {"name": "glgen_character_review", "description": "Render three fixed, bounds-framed review angles for a character job. At most three reviews are allowed before finalization; inspect the returned images before deciding on another edit.", "inputSchema": {"type": "object", "properties": {"jobId": {"type": "string"}}, "required": ["jobId"]}},
    {"name": "glgen_character_status", "description": "Read a persistent character job's brief, recipe, version history, limits, diagnostics, and current asset metrics.", "inputSchema": {"type": "object", "properties": {"jobId": {"type": "string"}}, "required": ["jobId"]}},
    {"name": "glgen_character_finalize", "description": "Validate a character job and export its editable recipe plus a static PBR GLB. Fails rather than exporting invalid, over-budget, or materially incomplete assets.", "inputSchema": {"type": "object", "properties": {"jobId": {"type": "string"}}, "required": ["jobId"]}},
])


def _character_job_path(job_id: str) -> str:
    if not job_id or any(c not in "0123456789abcdef" for c in job_id):
        raise GlGenError("invalid character job id")
    return os.path.join(CHARACTER_JOB_DIR, job_id)


def _save_character_job(job: Dict[str, Any]) -> None:
    directory = _character_job_path(job["jobId"])
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "job.json"), "w", encoding="utf-8") as handle:
        json.dump(job, handle, indent=2)
        handle.write("\n")
    with open(os.path.join(directory, f"revision_{job['version']:03d}.json"), "w", encoding="utf-8") as handle:
        json.dump(job["recipe"], handle, indent=2)
        handle.write("\n")


def _load_character_job(job_id: str) -> Dict[str, Any]:
    path = os.path.join(_character_job_path(job_id), "job.json")
    try:
        with open(path, encoding="utf-8") as handle:
            job = json.load(handle)
    except OSError as exc:
        raise GlGenError(f"unknown character job '{job_id}'") from exc
    if not isinstance(job, dict) or job.get("jobId") != job_id:
        raise GlGenError(f"corrupt character job '{job_id}'")
    return job


def _character_text(result: Dict[str, Any], prefix: str) -> Dict[str, Any]:
    text = f"{prefix}: {result['assetId']}, {result['triangles']} triangles"
    if result.get("warnings"):
        text += "\nWarnings:\n" + "\n".join("  - " + w for w in result["warnings"])
    return {"content": [text_block(text)]}


def build_generator_tools(engine: Engine) -> List[Dict[str, Any]]:
    """One tool per generator, carrying that generator's real parameter schema.

    This is what stops the model from guessing: the ranges, defaults and
    descriptions the engine validates against are the same ones it reads.
    """
    try:
        listing = engine.call("assets.generators")
    except GlGenError as exc:
        log(f"could not read generators ({exc}); serving static tools only")
        return []

    tools: List[Dict[str, Any]] = []
    for entry in listing.get("generators", []):
        name = entry["name"]
        try:
            detail = engine.call("assets.schema", {"name": name})
        except GlGenError:
            continue
        schema = detail.get("schema", {"type": "object", "properties": {}})
        properties = dict(schema.get("properties", {}))

        # Recipe-level fields every generator shares.
        properties["id"] = {
            "type": "string",
            "description": (
                "Stable name, e.g. 'tree/windswept_oak'. Reusing an id "
                "REGENERATES that asset in place, so anything already placed "
                "updates -- that is how you iterate."
            ),
        }
        properties["seed"] = {
            "type": "integer",
            "description": (
                "Random seed. Same parameters with a different seed gives a "
                "different individual of the same kind -- the cheap way to get "
                "variety without new meshes."
            ),
        }
        properties["material"] = {
            "type": "object",
            "description": (
                "Optional procedural textures, keyed by the material slot the "
                "generator emits (tree.v1: 'bark', 'foliage'; rock.v1: "
                "'rock'; kitbash.v1: 'prop'). Each value is "
                "{generator: 'tex.bark', params: {...}}. Texture generators: "
                + ", ".join(listing.get("textureGenerators", []))
                + "."
            ),
        }

        budget = entry.get("polyBudget", 0)
        budget_note = (
            f" Triangle budget: {budget}; exceeding it returns a warning."
            if budget
            else ""
        )
        tools.append(
            {
                "name": f"glgen_create_{sanitize(name)}",
                "description": (
                    f"{entry.get('description', name)}{budget_note} Returns an "
                    f"assetId -- render it with glgen_render_asset to see it."
                ),
                "inputSchema": {
                    "type": "object",
                    "properties": properties,
                    "required": ["id"],
                },
                "_generator": name,
            }
        )
    return tools


# --------------------------------------------------------------------------
# Tool execution
# --------------------------------------------------------------------------


def image_block(path: str) -> Optional[Dict[str, Any]]:
    """An MCP image content block, or None if the file is missing."""
    if not os.path.exists(path):
        return None
    with open(path, "rb") as handle:
        data = handle.read()
    return {
        "type": "image",
        "data": base64.b64encode(data).decode("ascii"),
        "mimeType": "image/png",
    }


def text_block(text: str) -> Dict[str, Any]:
    return {"type": "text", "text": text}


def call_tool(engine: Engine, tools: List[Dict[str, Any]], name: str,
              arguments: Dict[str, Any]) -> Dict[str, Any]:
    tool = next((t for t in tools if t["name"] == name), None)
    if tool is None:
        return {"content": [text_block(f"unknown tool '{name}'")], "isError": True}

    # -- generated per-generator creation tools ----------------------------
    if "_generator" in tool:
        args = dict(arguments)
        recipe = {
            "generator": tool["_generator"],
            "id": args.pop("id", None),
            "seed": args.pop("seed", 0),
            "material": args.pop("material", {}),
            "params": args,  # whatever remains belongs to the generator
        }
        result = engine.call("assets.define", recipe)
        SESSION_RECIPES[result["assetId"]] = copy.deepcopy(recipe)
        lines = [
            f"Created {result['assetId']}",
            f"{result['triangles']} triangles",
        ]
        if result.get("warnings"):
            # Warnings are the correction channel: surfacing them is what lets
            # the model fix its own parameters instead of inferring from pixels.
            lines.append("")
            lines.append("Warnings:")
            lines.extend(f"  - {w}" for w in result["warnings"])
        lines.append("")
        lines.append("Render it with glgen_render_asset to check how it looks.")
        return {"content": [text_block("\n".join(lines))]}

    # -- static tools -------------------------------------------------------
    if name == "glgen_character_begin":
        job_id = uuid.uuid4().hex
        target = int(arguments.get("targetTriangles", 15000))
        # A stable id is what lets every edit replace the mesh in place while
        # scene entities and a review turntable continue to reference it.
        slug = sanitize(arguments["name"].strip().lower().replace(" ", "_")) or "character"
        recipe = {
            "id": f"character/{slug}_{job_id[:8]}", "generator": "character.v1",
            "seed": int(arguments.get("seed", 0)),
            "params": {"operations": []},
            # These procedural sets are embedded into MeshData by AssetLibrary,
            # so character reviews exercise albedo/roughness/AO data instead
            # of a flat-colour placeholder even before an agent customizes it.
            "material": {
                "Skin": {"generator": "tex.leaf", "params": {"resolution": 128, "color": [0.72, 0.48, 0.34], "cutout": False}},
                "Jacket": {"generator": "tex.wood", "params": {"resolution": 128, "color": [0.08, 0.14, 0.28], "grain": 0.15}},
                "Shirt": {"generator": "tex.wood", "params": {"resolution": 128, "color": [0.85, 0.85, 0.82], "grain": 0.10}},
                "Trousers": {"generator": "tex.wood", "params": {"resolution": 128, "color": [0.12, 0.12, 0.16], "grain": 0.20}},
                "Belt": {"generator": "tex.metal", "params": {"resolution": 64, "color": [0.25, 0.15, 0.08], "polish": 0.30}},
                "Boots": {"generator": "tex.metal", "params": {"resolution": 64, "color": [0.32, 0.18, 0.09], "polish": 0.35}},
                "Hair": {"generator": "tex.wood", "params": {"resolution": 64, "color": [0.05, 0.04, 0.04], "grain": 0.70}},
            },
            "provenance": {"brief": arguments["brief"], "author": "mcp-character-v1"},
        }
        result = engine.call("assets.define", recipe)
        job = {"jobId": job_id, "brief": arguments["brief"], "name": arguments["name"],
               "targetTriangles": target, "version": 0, "edits": 0, "reviews": 0,
               "assetId": result["assetId"], "recipe": recipe,
               "history": [{"version": 0, "action": "begin", "warnings": result.get("warnings", [])}]}
        _save_character_job(job)
        reply = _character_text(result, f"Started character job {job_id}")
        reply["content"].append(text_block("Use glgen_character_review, then apply targeted semantic edits with expectedVersion 0."))
        return reply

    if name == "glgen_character_status":
        job = _load_character_job(arguments["jobId"])
        info = engine.call("assets.info", {"assetId": job["assetId"]})
        return {"content": [text_block(json.dumps({**job, "metrics": info}, indent=2))]}

    if name == "glgen_character_edit":
        job = _load_character_job(arguments["jobId"])
        if int(arguments["expectedVersion"]) != int(job["version"]):
            return {"content": [text_block(f"stale character revision: expected {arguments['expectedVersion']}, current is {job['version']}")], "isError": True}
        if job["edits"] >= 8:
            return {"content": [text_block("character job exhausted its eight-edit budget; review or start a new job")], "isError": True}
        operation = arguments["operation"]
        if not isinstance(operation, dict) or not isinstance(operation.get("op"), str):
            return {"content": [text_block("character operation needs a string 'op'")], "isError": True}
        allowed = {"reshape", "add_garment", "add_feature", "add_hair", "add_accessory",
                   "assign_material", "mirror", "smooth", "remesh", "simplify"}
        if operation["op"] not in allowed:
            return {"content": [text_block("unsupported character operation '" + operation["op"] + "'")], "isError": True}
        recipe = copy.deepcopy(job["recipe"])
        recipe["params"].setdefault("operations", []).append(operation)
        result = engine.call("assets.define", recipe)
        job["recipe"] = recipe
        job["assetId"] = result["assetId"]
        job["edits"] += 1
        job["version"] += 1
        job["history"].append({"version": job["version"], "action": operation, "warnings": result.get("warnings", [])})
        _save_character_job(job)
        return _character_text(result, f"Character revision {job['version']}")

    if name == "glgen_character_review":
        job = _load_character_job(arguments["jobId"])
        if job["reviews"] >= 3:
            return {"content": [text_block("character job exhausted its three-review budget")], "isError": True}
        review_dir = _character_job_path(job["jobId"])
        base = os.path.join(review_dir, f"review_{job['version']:03d}")
        result = engine.call("render.turntable", {"assetId": job["assetId"], "path": base, "steps": 3})
        job["reviews"] += 1
        job["history"].append({"version": job["version"], "action": "review", "paths": result["paths"]})
        _save_character_job(job)
        content: List[Dict[str, Any]] = [text_block(f"Character review {job['reviews']}/3, revision {job['version']}:")]
        for path in result["paths"]:
            block = image_block(path)
            if block: content.append(block)
        return {"content": content}

    if name == "glgen_character_finalize":
        job = _load_character_job(arguments["jobId"])
        info = engine.call("assets.info", {"assetId": job["assetId"]})
        if info["triangles"] > job["targetTriangles"]:
            return {"content": [text_block(f"cannot finalize: {info['triangles']} triangles exceeds target {job['targetTriangles']}")], "isError": True}
        if info["submeshes"] < 2:
            return {"content": [text_block("cannot finalize: character is missing required material regions")], "isError": True}
        directory = _character_job_path(job["jobId"])
        recipe_path = os.path.join(directory, "character_recipe.json")
        glb_path = os.path.join(directory, "character.glb")
        with open(recipe_path, "w", encoding="utf-8") as handle:
            json.dump(job["recipe"], handle, indent=2)
            handle.write("\n")
        engine.call("assets.exportGlb", {"assetId": job["assetId"], "path": glb_path})
        job["finalized"] = {"recipe": recipe_path, "glb": glb_path, "metrics": info}
        job["history"].append({"version": job["version"], "action": "finalize"})
        _save_character_job(job)
        return {"content": [text_block(json.dumps(job["finalized"], indent=2))]}

    if name == "glgen_info":
        info = engine.call("engine.info")
        return {"content": [text_block(json.dumps(info, indent=2))]}

    if name == "glgen_modify_asset" or name == "glgen_apply_material":
        asset_id = arguments["assetId"]
        previous = SESSION_RECIPES.get(asset_id)
        if previous is None:
            return {"content": [text_block(
                "This asset was not created in this MCP session, so its recipe "
                "is unavailable for a safe edit. Create it again with a stable id "
                "or use glgen_eval_lua.")], "isError": True}
        recipe = copy.deepcopy(previous)
        if name == "glgen_modify_asset":
            if "params" in arguments:
                recipe["params"].update(arguments["params"])
            if "seed" in arguments:
                recipe["seed"] = arguments["seed"]
        if "material" in arguments:
            recipe["material"].update(arguments["material"])
        result = engine.call("assets.define", recipe)
        # AssetLibrary's id is stable, but retain the returned spelling rather
        # than assuming a generator will never change its asset-id convention.
        SESSION_RECIPES.pop(asset_id, None)
        SESSION_RECIPES[result["assetId"]] = recipe
        text = f"Updated {result['assetId']}: {result['triangles']} triangles"
        if result.get("warnings"):
            text += "\nWarnings:\n" + "\n".join("  - " + w for w in result["warnings"])
        return {"content": [text_block(text)]}

    if name == "glgen_create_assembly":
        base = arguments.get("pos", [0.0, 0.0, 0.0])
        placed = []
        for index, part in enumerate(arguments["parts"]):
            local = part.get("pos", [0.0, 0.0, 0.0])
            if len(local) != 3:
                return {"content": [text_block(f"assembly part {index} has invalid pos")], "isError": True}
            spawn = {"assetId": part["assetId"],
                     "pos": [base[i] + local[i] for i in range(3)],
                     "rot": part.get("rot", [0.0, 0.0, 0.0]),
                     "scale": part.get("scale", 1.0), "onGround": False,
                     "name": f"{arguments['name']}/{part.get('name', index)}"}
            placed.append(engine.call("scene.spawn", spawn))
        return {"content": [text_block(json.dumps({"assembly": arguments["name"], "parts": placed}, indent=2))]}

    if name == "glgen_validate_asset":
        info = engine.call("assets.info", {"assetId": arguments["assetId"]})
        mn, mx = info["boundsMin"], info["boundsMax"]
        extent = [round(mx[i] - mn[i], 4) for i in range(3)]
        notes: List[str] = []
        if info["triangles"] > 10000:
            notes.append("high unique-mesh triangle count; create an LOD before scattering")
        if min(extent) < 0.002:
            notes.append("very thin axis; check silhouette and ray-traced shadows")
        if not notes:
            notes.append("bounds and mesh metrics are within normal authoring ranges")
        return {"content": [text_block(json.dumps({**info, "extent": extent,
                                                      "guidance": notes}, indent=2))]}

    if name == "glgen_render_compare":
        os.makedirs(CAPTURE_DIR, exist_ok=True)
        views = int(arguments.get("views", 2))
        content: List[Dict[str, Any]] = []
        for label, asset_id in (("Before", arguments["beforeAssetId"]),
                                ("After", arguments["afterAssetId"])):
            info = engine.call("assets.info", {"assetId": asset_id})
            content.append(text_block(f"{label}: {asset_id}\n" + json.dumps(info, indent=2)))
            base = os.path.join(CAPTURE_DIR, f"compare_{label.lower()}_{sanitize(asset_id.split('/')[-1])}")
            result = engine.call("render.turntable", {"assetId": asset_id, "path": base, "steps": views})
            for path in result["paths"]:
                block = image_block(path)
                if block:
                    content.append(block)
        return {"content": content}

    if name == "glgen_create_lod_set":
        source_id = arguments["assetId"]
        source = SESSION_RECIPES.get(source_id)
        if source is None or "detail" not in source.get("params", {}):
            return {"content": [text_block(
                "LOD creation currently requires a session-created recipe with a "
                "numeric 'detail' parameter (such as sculpt.v1 or rock.v1).")], "isError": True}
        count = int(arguments.get("levels", 2))
        original = int(source["params"]["detail"])
        rows = []
        for level in range(1, count + 1):
            recipe = copy.deepcopy(source)
            recipe["id"] = f"{source['id']}/lod{level}"
            recipe["params"]["detail"] = max(0, original - level)
            result = engine.call("assets.define", recipe)
            SESSION_RECIPES[result["assetId"]] = recipe
            rows.append({"level": level, "assetId": result["assetId"],
                         "detail": recipe["params"]["detail"],
                         "triangles": result["triangles"], "warnings": result.get("warnings", [])})
        return {"content": [text_block(json.dumps({"source": source_id, "lods": rows}, indent=2))]}

    if name == "glgen_import_reference":
        recipe = {"id": arguments["id"], "generator": "import.gltf", "seed": 0,
                  "params": {"path": arguments["path"],
                             "maxTriangles": arguments.get("maxTriangles", 8000),
                             "normalizeSize": arguments.get("normalizeSize", 1.0)}}
        result = engine.call("assets.define", recipe)
        SESSION_RECIPES[result["assetId"]] = recipe
        return {"content": [text_block(f"Imported {result['assetId']}: {result['triangles']} triangles" +
                                         ("\nWarnings:\n" + "\n".join("  - " + w for w in result["warnings"])
                                          if result.get("warnings") else ""))]}

    if name == "glgen_render_asset":
        os.makedirs(CAPTURE_DIR, exist_ok=True)
        asset_id = arguments["assetId"]
        views = int(arguments.get("views", 3))
        base = os.path.join(CAPTURE_DIR, sanitize(asset_id.split("/")[-1]))
        result = engine.call(
            "render.turntable",
            {"assetId": asset_id, "path": base, "steps": views},
        )
        content: List[Dict[str, Any]] = [
            text_block(f"{views} view(s) of {asset_id}:")
        ]
        for path in result["paths"]:
            block = image_block(path)
            if block:
                content.append(block)
            else:
                content.append(text_block(f"(missing capture {path})"))
        return {"content": content}

    if name == "glgen_screenshot":
        os.makedirs(CAPTURE_DIR, exist_ok=True)
        path = os.path.join(CAPTURE_DIR, "view.png")
        engine.call(
            "render.capture",
            {"path": path, "includeUi": bool(arguments.get("includeUi", False))},
        )
        block = image_block(path)
        return {
            "content": [block] if block else [text_block("capture failed")],
            **({} if block else {"isError": True}),
        }

    if name == "glgen_place_asset":
        result = engine.call("scene.spawn", arguments)
        return {
            "content": [
                text_block(
                    f"Placed entity {result['entityId']} at "
                    f"{[round(v, 2) for v in result['pos']]}"
                )
            ]
        }

    if name == "glgen_scene_query":
        result = engine.call("scene.query", arguments)
        return {"content": [text_block(json.dumps(result, indent=2))]}

    if name == "glgen_scene_clear":
        result = engine.call("scene.clear", arguments)
        return {"content": [text_block(f"Removed {result['destroyed']} entities")]}

    if name == "glgen_set_render_params":
        result = engine.call("render.setParams", arguments)
        return {"content": [text_block(json.dumps(result, indent=2))]}

    if name == "glgen_eval_lua":
        result = engine.call("script.eval", {"code": arguments["code"]})
        return {"content": [text_block(json.dumps(result["value"], indent=2))]}

    return {"content": [text_block(f"unhandled tool '{name}'")], "isError": True}


# --------------------------------------------------------------------------
# MCP transport (newline-delimited JSON-RPC over stdio)
# --------------------------------------------------------------------------


class McpServer:
    def __init__(self, engine: Engine) -> None:
        self.engine = engine
        self.tools: List[Dict[str, Any]] = list(STATIC_TOOLS)
        self._tools_built = False

    def ensure_tools(self) -> None:
        """Generator tools need the engine, so they are built on first use
        rather than at startup -- a client that only calls initialize should
        not launch the engine."""
        if self._tools_built:
            return
        self.tools = list(STATIC_TOOLS) + build_generator_tools(self.engine)
        self._tools_built = True

    def handle(self, message: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        method = message.get("method")
        msg_id = message.get("id")
        params = message.get("params") or {}

        if method == "initialize":
            return self._ok(msg_id, {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                # How to operate this server. Per-tool descriptions cover one
                # call each; this is where the workflow lives.
                "instructions": SERVER_INSTRUCTIONS,
            })

        if method in ("notifications/initialized", "initialized"):
            return None  # notification: no reply

        if method == "ping":
            return self._ok(msg_id, {})

        if method == "tools/list":
            self.ensure_tools()
            public = [
                {k: v for k, v in tool.items() if not k.startswith("_")}
                for tool in self.tools
            ]
            return self._ok(msg_id, {"tools": public})

        if method == "tools/call":
            self.ensure_tools()
            name = params.get("name", "")
            arguments = params.get("arguments") or {}
            try:
                result = call_tool(self.engine, self.tools, name, arguments)
            except GlGenError as exc:
                # Engine-level failures come back as tool errors, not protocol
                # errors: the model can read and act on them.
                result = {"content": [text_block(f"Engine error: {exc}")],
                          "isError": True}
            except Exception as exc:  # noqa: BLE001
                result = {"content": [text_block(f"Tool failed: {exc}")],
                          "isError": True}
            return self._ok(msg_id, result)

        if msg_id is None:
            return None  # unknown notification
        return {
            "jsonrpc": "2.0",
            "id": msg_id,
            "error": {"code": -32601, "message": f"unknown method '{method}'"},
        }

    @staticmethod
    def _ok(msg_id: Any, result: Dict[str, Any]) -> Dict[str, Any]:
        return {"jsonrpc": "2.0", "id": msg_id, "result": result}

    def serve_stdio(self) -> None:
        log("serving on stdio")
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError as exc:
                log(f"bad JSON from client: {exc}")
                continue
            response = self.handle(message)
            if response is not None:
                sys.stdout.write(json.dumps(response) + "\n")
                sys.stdout.flush()


# --------------------------------------------------------------------------


def selftest() -> int:
    """Exercises the server in-process, without an MCP client."""
    engine = Engine()
    server = McpServer(engine)
    failures = 0

    def check(label: str, condition: bool, detail: str = "") -> None:
        nonlocal failures
        if condition:
            print(f"  ok    {label}")
        else:
            failures += 1
            print(f"  FAIL  {label}  {detail}")

    print("[initialize]")
    init = server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize"})
    check("protocol version", init["result"]["protocolVersion"] == PROTOCOL_VERSION)
    check("tools capability", "tools" in init["result"]["capabilities"])
    # The workflow guidance only reaches a model if it is actually sent.
    instructions = init["result"].get("instructions", "")
    check("instructions sent", len(instructions) > 500, f"{len(instructions)} chars")
    check("instructions cover the loop", "glgen_render_asset" in instructions)
    check("instructions cover id reuse", "REUSING THE SAME id" in instructions)

    print("\n[tools/list]")
    listing = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    tools = listing["result"]["tools"]
    names = [t["name"] for t in tools]
    check("static tools present", "glgen_render_asset" in names)
    check("generator tools generated", "glgen_create_tree_v1" in names, str(names))
    check("no private keys leak", all("_generator" not in t for t in tools))

    tree = next((t for t in tools if t["name"] == "glgen_create_tree_v1"), None)
    check("tree tool exists", tree is not None)
    if tree:
        props = tree["inputSchema"]["properties"]
        check("real schema inlined", "height" in props and "canopy" in props)
        check("ranges preserved", props["height"].get("maximum") == 40,
              str(props["height"]))
        check("enum preserved", "conifer" in props["canopy"].get("enum", []))
        check("recipe fields added", "id" in props and "seed" in props)
        print(f"  tree.v1 exposes {len(props)} parameters")

    print("\n[tools/call — create]")
    created = server.handle({
        "jsonrpc": "2.0", "id": 3, "method": "tools/call",
        "params": {
            "name": "glgen_create_rock_v1",
            "arguments": {
                "id": "rock/mcp_test", "seed": 99, "radius": 0.9,
                "detail": 2, "roughness": 0.4,
                "material": {"rock": {"generator": "tex.rock",
                                      "params": {"resolution": 128}}},
            },
        },
    })
    text = created["result"]["content"][0]["text"]
    check("asset created", "gen://rock.v1/rock/mcp_test" in text, text)
    check("triangles reported", "triangles" in text)
    print("  " + text.splitlines()[0])

    print("\n[tools/call — warnings reach the model]")
    clamped = server.handle({
        "jsonrpc": "2.0", "id": 4, "method": "tools/call",
        "params": {"name": "glgen_create_rock_v1",
                   "arguments": {"id": "rock/mcp_big", "radius": 999}},
    })
    check("clamp warning surfaced",
          "clamped" in clamped["result"]["content"][0]["text"],
          clamped["result"]["content"][0]["text"])

    print("\n[tools/call — render returns images]")
    rendered = server.handle({
        "jsonrpc": "2.0", "id": 5, "method": "tools/call",
        "params": {"name": "glgen_render_asset",
                   "arguments": {"assetId": "gen://rock.v1/rock/mcp_test",
                                 "views": 2}},
    })
    content = rendered["result"]["content"]
    images = [c for c in content if c["type"] == "image"]
    check("images returned", len(images) >= 2, str(len(images)))
    if images:
        size_kb = len(images[0]["data"]) / 1024
        check("image is base64 png", images[0]["mimeType"] == "image/png")
        # Downscaling happens in the engine; if this regresses, every render
        # floods the model's context.
        check("image small enough to inline", size_kb < 700, f"{size_kb:.0f} KB")
        print(f"  each image ~{size_kb:.0f} KB base64")

    print("\n[tools/call — errors are tool errors, not crashes]")
    bad = server.handle({
        "jsonrpc": "2.0", "id": 6, "method": "tools/call",
        "params": {"name": "glgen_render_asset",
                   "arguments": {"assetId": "gen://nope/nope"}},
    })
    check("bad asset reported", bad["result"].get("isError") is True,
          str(bad["result"])[:120])

    print("\n[unknown method]")
    unknown = server.handle({"jsonrpc": "2.0", "id": 7, "method": "nope"})
    check("unknown method rejected", "error" in unknown)

    engine.close()
    print()
    if failures:
        print(f"FAILED: {failures} check(s)")
        return 1
    print("all checks passed")
    return 0


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    engine = Engine()
    try:
        McpServer(engine).serve_stdio()
    except KeyboardInterrupt:
        pass
    finally:
        engine.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

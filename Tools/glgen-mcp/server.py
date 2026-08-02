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
import json
import os
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "glgen-client")
)
from glgen_client import GlGenClient, GlGenError  # noqa: E402

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "glgen"
SERVER_VERSION = "0.1.0"

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
DEFAULT_PORT = int(os.environ.get("GLGEN_AGENT_PORT", "8787"))
DEFAULT_EXE = os.environ.get(
    "GLGEN_EXE", os.path.join(REPO_ROOT, "Build-vs18", "bin", "Release", "glGenVk.exe")
)
CAPTURE_DIR = os.path.join(REPO_ROOT, "captures", "mcp")


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
                "fogDensity": {"type": "number"},
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
    if name == "glgen_info":
        info = engine.call("engine.info")
        return {"content": [text_block(json.dumps(info, indent=2))]}

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
    check("two images returned", len(images) == 2, str(len(images)))
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

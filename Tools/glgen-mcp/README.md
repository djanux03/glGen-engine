# glGen MCP server

Lets an AI client author 3D content in glGen and **see the result** — generate
an asset from parameters, render it, look at the image, adjust, repeat.

## Setup

Nothing to install: it uses only the Python standard library (see "Why no SDK"
below). Point your MCP client at it:

```json
{
  "mcpServers": {
    "glgen": {
      "command": "python",
      "args": ["C:/Users/you/glGen-main/Tools/glgen-mcp/server.py"],
      "env": {
        "GLGEN_AGENT_PORT": "8787",
        "GLGEN_EXE": "C:/Users/you/glGen-main/Build-vs18/bin/Release/glGenVk.exe"
      }
    }
  }
}
```

If the engine is already running with `GLGEN_AGENT_PORT` set, the server
connects to it. Otherwise it launches `GLGEN_EXE` and waits for the port.

Check it without a client:

```bash
python Tools/glgen-mcp/server.py --selftest
```

## Tools

Eight are fixed; the rest are **generated from the engine's own generator
schemas** at connect time, one `glgen_create_<generator>` per generator.

| Tool | What it does |
|---|---|
| `glgen_info` | engine state, generators, loaded recipes |
| `glgen_create_tree_v1` *(generated)* | 21 parameters with real ranges and descriptions |
| `glgen_create_rock_v1` *(generated)* | … and one per other generator |
| `glgen_render_asset` | orbits the asset, **returns images** |
| `glgen_screenshot` | current view, as an image |
| `glgen_place_asset` | put it in the world, optionally on the terrain |
| `glgen_scene_query` / `glgen_scene_clear` | inspect / tidy |
| `glgen_set_render_params` | camera, sun, exposure, fog |
| `glgen_eval_lua` | escape hatch for anything else |

Because a generated tool's `inputSchema` **is** the engine's validation schema,
the model reads `tree.v1`'s real bounds (`height: 0.3 … 40`, `canopy:
conifer | broadleaf | blob | bare`) before its first call, rather than guessing
and being corrected. There is no second copy to drift.

## The loop

```
glgen_create_tree_v1 { id: "tree/oak", height: 6, branchAngleDeg: 80, ... }
    -> "Created gen://tree.v1/tree/oak, 400 triangles"
glgen_render_asset  { assetId: "gen://tree.v1/tree/oak" }
    -> [image] branches stick out sideways, almost no leaves
glgen_create_tree_v1 { id: "tree/oak", branchAngleDeg: 42, canopyDensity: 0.85, ... }
    -> "Warnings: generated 5520 triangles, over the 4000 budget"
glgen_render_asset  { ... }
    -> [image] recognisable tree; lower the detail to clear the budget
```

Reusing an `id` regenerates that asset **in place**, so anything already placed
in the scene updates too. That is what makes iteration cheap.

Feedback arrives on two channels and both matter: the **image** shows
proportion and silhouette, while **text warnings** carry what an image cannot —
clamped parameters, poly budgets, material slots that matched nothing.

## Notes

**Why no SDK.** MCP's stdio transport is newline-delimited JSON-RPC 2.0, the
same protocol the engine's command port already speaks, and the surface needed
here is just `initialize` / `tools/list` / `tools/call`. Standard library only
means `python server.py` works with no install step. If this grows resources,
prompts or sampling, switch to the official `mcp` package rather than growing
`server.py`.

**Images are downscaled in the engine**, not here (`requestCapture`'s
`maxDimension`, default 768 px). A native capture is ~1.4 MB, about 1.9 MB once
base64-encoded — unusable at several frames per render. Downscaling at the
source also avoids a Python image dependency.

**Renders hide the editor UI.** Docked ImGui panels cover roughly a third of
the frame; a review shot should show the scene, not the tool.

**Security.** The command port runs arbitrary Lua in the engine process. It
binds loopback only and is off unless `GLGEN_AGENT_PORT` is set. Do not expose
it.

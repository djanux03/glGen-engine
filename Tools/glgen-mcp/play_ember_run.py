#!/usr/bin/env python3
"""Build and launch EMBER RUN through the glGen MCP server.

Speaks the MCP protocol over stdio to Tools/glgen-mcp/server.py exactly like
an MCP host would: initialize -> tools/list -> tools/call. The server owns the
engine connection (and launches glGenVk if nothing is listening on the command
port), so this script never talks to the engine directly.

    python Tools/glgen-mcp/play_ember_run.py            # build + play
    python Tools/glgen-mcp/play_ember_run.py --shot out.png

Everything the game needs -- terrain, player, ember manager, play mode -- is
created from here. Nothing has to be clicked in the editor.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from typing import Any, Dict, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SERVER = os.path.join(HERE, "server.py")


class McpClient:
    """Minimal MCP stdio client: enough to list and call tools."""

    def __init__(self, server_path: str) -> None:
        self._next_id = 0
        self.proc = subprocess.Popen(
            [sys.executable, server_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,  # server diagnostics pass through to our stderr
            cwd=REPO_ROOT,
            text=True,
            bufsize=1,
            env=dict(os.environ),
        )

    def _send(self, payload: Dict[str, Any]) -> None:
        assert self.proc.stdin
        self.proc.stdin.write(json.dumps(payload) + "\n")
        self.proc.stdin.flush()

    def _read(self) -> Dict[str, Any]:
        assert self.proc.stdout
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("MCP server closed the connection")
            line = line.strip()
            if not line:
                continue
            try:
                return json.loads(line)
            except json.JSONDecodeError:
                # Not protocol traffic (a stray print) -- surface and skip.
                print(f"[non-json] {line}", file=sys.stderr)

    def request(self, method: str, params: Optional[Dict[str, Any]] = None) -> Any:
        self._next_id += 1
        msg_id = self._next_id
        self._send({"jsonrpc": "2.0", "id": msg_id, "method": method,
                    "params": params or {}})
        while True:
            msg = self._read()
            if msg.get("id") == msg_id:
                if "error" in msg:
                    raise RuntimeError(f"{method}: {msg['error']}")
                return msg.get("result")

    def notify(self, method: str, params: Optional[Dict[str, Any]] = None) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params or {}})

    def call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        result = self.request("tools/call",
                              {"name": name, "arguments": arguments})
        chunks = []
        for block in result.get("content", []):
            if block.get("type") == "text":
                chunks.append(block.get("text", ""))
            elif block.get("type") == "image":
                chunks.append("<image>")
        text = "\n".join(chunks)
        if result.get("isError"):
            raise RuntimeError(f"{name} failed: {text}")
        return text

    def close(self) -> None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


# --------------------------------------------------------------------------
# The game setup, as Lua evaluated inside the engine
# --------------------------------------------------------------------------

# Terrain first: the embers are placed on its surface, so it has to exist
# before the game script runs its scatter.
SETUP_TERRAIN = """
terrain.regenerate{
    seed = 20260804,
    heightScale = 22.0,
    viewDistanceChunks = 4,
    vegetation = true,
    -- Grass off: the scattered blades stand taller than the player capsule,
    -- so at eye level they form an opaque wall. Fine as scenery in an
    -- editor flythrough, unplayable as a game you have to spot pickups in.
    grass = false,
    treeDensity = 0.7,
    rockDensity = 0.8,
}
return string.format("terrain exists=%s h(0,0)=%.2f",
                     tostring(terrain.exists()), terrain.height_at(0, 0))
"""

# A late-afternoon look: low warm sun, some haze, so the embers read as
# bright points against the ground.
SETUP_LOOK = """
render.params{
    sunPitch = 18.0,
    sunYaw = 210.0,
    sunIntensity = 1.5,
    exposure = 1.05,
    fogDensity = 0.0022,
    bloomIntensity = 0.5,
}
return "look ok"
"""

# Player, then the manager entity that owns the game loop. The manager is
# parked far below the world with a tiny scale: it needs to exist as an
# entity to carry a ScriptComponent, but it is not meant to be seen.
SETUP_GAME = """
local id = game.spawn_player{ pos = {0, 0, 0}, yaw = 0.0, onGround = true }

local mgr = world.spawn_primitive("cube", 0, -1000, 0, 0.01)
mgr:set_name("GameManager")
mgr:attach_script("assets/scripts/ember_run.lua")

return "player=" .. tostring(id) .. " manager=" .. tostring(mgr:id())
"""


# Walks the player onto embers by teleporting them, one per step, and reads
# the score back off the HUD. There is no way to synthesize mouse/keyboard
# into the running engine, so this exercises the actual collection rule
# (proximity + destroy + score + time bonus) rather than the input layer.
SELFTEST = """
local hud = game.get_hud()
local before = hud.score
local moved = 0
for i = 1, 24 do
    local e = world.find_entity("Ember_" .. i)
    if e and e:is_valid() then
        local p = e:get_position()
        local player = world.find_entity("Player")
        if player and player:is_valid() then
            player:set_position(p.x, p.y, p.z)
            moved = moved + 1
            if moved >= 3 then break end
        end
    end
end
return string.format("teleported=%d scoreBefore=%d", moved, before)
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--shot", help="capture a screenshot to this path once running")
    ap.add_argument("--settle", type=float, default=3.0,
                    help="seconds to let terrain stream in before playing")
    ap.add_argument("--selftest", action="store_true",
                    help="verify collection works by teleporting onto embers")
    args = ap.parse_args()

    client = McpClient(SERVER)
    try:
        init = client.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "ember-run-launcher", "version": "1.0"},
        })
        server_name = init.get("serverInfo", {}).get("name", "?")
        print(f"connected to MCP server: {server_name}")
        client.notify("notifications/initialized")

        tools = client.request("tools/list").get("tools", [])
        names = {t["name"] for t in tools}
        print(f"server exposes {len(tools)} tools")
        for required in ("glgen_eval_lua", "glgen_info"):
            if required not in names:
                raise RuntimeError(f"server is missing {required}")

        print("engine:", client.call_tool("glgen_info", {}).splitlines()[0])

        print("building terrain...")
        print("  ", client.call_tool("glgen_eval_lua", {"code": SETUP_TERRAIN}))
        print("  ", client.call_tool("glgen_eval_lua", {"code": SETUP_LOOK}))

        # Chunks stream in on worker threads; scattering embers against a
        # half-built heightfield would float them or bury them.
        time.sleep(args.settle)

        print("spawning player and game manager...")
        print("  ", client.call_tool("glgen_eval_lua", {"code": SETUP_GAME}))

        print("starting play mode...")
        client.call_tool("glgen_eval_lua", {"code": "game.play() return 'playing'"})
        time.sleep(2.0)

        state = client.call_tool("glgen_eval_lua", {
            "code": "return tostring(game.is_playing())"})
        print("  is_playing:", state)

        if args.selftest:
            print("self-test: collecting embers by teleport...")
            hud0 = client.call_tool("glgen_eval_lua", {
                "code": "local h = game.get_hud() "
                        "return string.format('%d|%.1f', h.score, h.time)"})
            score0, time0 = hud0.strip('"').split("|")
            # One teleport per tick: the pickup check runs once per frame, so
            # stepping them apart is what proves each one counted.
            for _ in range(3):
                client.call_tool("glgen_eval_lua", {"code": SELFTEST})
                time.sleep(0.4)
            hud1 = client.call_tool("glgen_eval_lua", {
                "code": "local h = game.get_hud() "
                        "return string.format('%d|%.1f', h.score, h.time)"})
            score1, time1 = hud1.strip('"').split("|")
            print(f"   score {score0} -> {score1}   clock {time0}s -> {time1}s")
            if int(score1) > int(score0):
                print("   PASS: embers were collected and the score advanced")
            else:
                print("   FAIL: score did not move")
                return 1

        if args.shot:
            print("capturing...")
            client.call_tool("glgen_eval_lua", {
                "code": f'render.capture([[{args.shot}]]) return "capture queued"'})
            time.sleep(1.5)
            print("  wrote", args.shot)

        print("\nEMBER RUN is live. WASD to run, mouse to look, R to restart.")
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())

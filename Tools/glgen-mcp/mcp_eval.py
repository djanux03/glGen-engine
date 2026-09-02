#!/usr/bin/env python3
"""Run Lua inside glGen through the MCP server.

    python Tools/glgen-mcp/mcp_eval.py "return scene.stats()"
    echo "return 1+1" | python Tools/glgen-mcp/mcp_eval.py -

Connects to a running engine if one is listening, otherwise the server starts
one. Handy for poking at a live scene without writing a bespoke client.
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from play_ember_run import HERE, McpClient, SERVER  # noqa: E402


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    code = sys.stdin.read() if sys.argv[1] == "-" else " ".join(sys.argv[1:])

    client = McpClient(SERVER)
    try:
        client.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "mcp-eval", "version": "1.0"},
        })
        client.notify("notifications/initialized")
        print(client.call_tool("glgen_eval_lua", {"code": code}))
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())

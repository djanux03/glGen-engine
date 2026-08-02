"""Minimal JSON-RPC client for glGen's command port.

The engine listens on 127.0.0.1 (see Engine/Bridge/CommandServer.h) speaking
newline-delimited JSON-RPC 2.0. This is the reference client: the Phase 4 MCP
server wraps it, and it is useful on its own for scripting and CI.

Start the engine with the port open:

    GLGEN_AGENT_PORT=8787 Build-vs18/bin/Release/glGenVk.exe

Then:

    from glgen_client import GlGenClient
    with GlGenClient() as c:
        print(c.call("engine.info"))
"""

from __future__ import annotations

import json
import socket
from typing import Any, Dict, Optional


class GlGenError(RuntimeError):
    """The engine returned a JSON-RPC error."""


class GlGenClient:
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 8787,
        timeout: float = 60.0,
    ) -> None:
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(timeout)
        self._buffer = b""
        self._next_id = 1
        # Replies can arrive out of order: a deferred request (render.capture,
        # render.turntable) is answered frames later, so a call issued after it
        # may well be answered first. Anything not being waited on lands here.
        self._pending: Dict[int, Any] = {}

    # -- context manager ----------------------------------------------------
    def __enter__(self) -> "GlGenClient":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    # -- protocol -----------------------------------------------------------
    def call(self, method: str, params: Optional[Dict[str, Any]] = None) -> Any:
        """Send a request and block until its reply arrives."""
        request_id = self._next_id
        self._next_id += 1
        payload = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": params or {},
        }
        self._sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
        return self._await(request_id)

    def notify(self, method: str, params: Optional[Dict[str, Any]] = None) -> None:
        """Fire-and-forget: no id, so the engine sends no reply."""
        payload = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        self._sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))

    def _await(self, request_id: int) -> Any:
        if request_id in self._pending:
            return self._unwrap(self._pending.pop(request_id))
        while True:
            message = self._read_message()
            if message.get("id") == request_id:
                return self._unwrap(message)
            if "id" in message:
                self._pending[message["id"]] = message

    def _read_message(self) -> Dict[str, Any]:
        while b"\n" not in self._buffer:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise GlGenError("connection closed by the engine")
            self._buffer += chunk
        line, self._buffer = self._buffer.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    @staticmethod
    def _unwrap(message: Dict[str, Any]) -> Any:
        if "error" in message:
            raise GlGenError(message["error"].get("message", "unknown error"))
        return message.get("result")

    # -- conveniences -------------------------------------------------------
    def eval(self, code: str) -> Any:
        """Run Lua and return its value. The general escape hatch: everything
        bound in Phase 2 is reachable without a typed method here."""
        return self.call("script.eval", {"code": code})["value"]

    def define(self, recipe: Dict[str, Any]) -> Dict[str, Any]:
        return self.call("assets.define", recipe)

    def spawn(self, asset_id: str, **kwargs: Any) -> Dict[str, Any]:
        return self.call("scene.spawn", {"assetId": asset_id, **kwargs})

    def capture(self, path: str) -> str:
        """Blocks until the PNG is on disk (the reply is deferred until the
        frame that wrote it has completed)."""
        return self.call("render.capture", {"path": path})["path"]

    def turntable(self, asset_id: str, path: str, steps: int = 6) -> list:
        return self.call(
            "render.turntable",
            {"assetId": asset_id, "path": path, "steps": steps},
        )["paths"]

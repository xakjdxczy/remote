"""Signaling server and static web UI. Media is P2P-only and is never relayed."""

from __future__ import annotations

import asyncio
import logging
import os
import re
import time
from pathlib import Path
from typing import Any

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from remote.ids import constant_time_equals, format_device_id, normalize_device_id
from remote.p2p import SIGNAL_KINDS, ice_servers_payload
from remote.protocol import decode_json, encode_json
from remote.server.registry import Registry

logger = logging.getLogger("remotedesk.server")
WEB_DIR = Path(__file__).resolve().parent.parent / "web"

registry = Registry()
demo_host: dict[str, str] | None = None
SERVER_START = time.time()


def _net_bytes() -> int | None:
    """Total NIC bytes (rx+tx) across non-loopback interfaces, from /proc/net/dev.

    Reflects the server's real network usage (signaling + TURN relay + web).
    """
    try:
        total = 0
        with open("/proc/net/dev") as fh:
            for line in fh.readlines()[2:]:
                iface, _, data = line.partition(":")
                if iface.strip() == "lo":
                    continue
                parts = data.split()
                if len(parts) >= 9:
                    total += int(parts[0]) + int(parts[8])  # rx bytes + tx bytes
        return total
    except Exception:
        return None


NET_START = _net_bytes() or 0

_RELAY_LOG = os.environ.get("COTURN_LOG", "/var/log/turnserver/turn.log")
_relay_state = {"offset": 0, "total": 0}
_relay_re = re.compile(r"rb=(\d+).*?sb=(\d+)")


def _scrape_coturn_bytes() -> int | None:
    """Cumulative TURN-relayed bytes, parsed from coturn's usage log.

    The signaling server carries no media (P2P), so the only server-side data
    traffic is the TURN relay. coturn logs a ``usage: ... rb=.. sb=..`` line per
    session; we sum rb+sb incrementally. Returns None when no log is available
    (TURN disabled / no coturn), so the UI can degrade gracefully. Note: usage
    lines are emitted at session close, so this is cumulative, not instantaneous.
    """
    try:
        if not os.path.exists(_RELAY_LOG):
            return None
        size = os.path.getsize(_RELAY_LOG)
        if size < _relay_state["offset"]:  # rotated/truncated
            _relay_state["offset"] = 0
            _relay_state["total"] = 0
        with open(_RELAY_LOG, "r", errors="replace") as fh:
            fh.seek(_relay_state["offset"])
            for line in fh:
                if "usage" in line:
                    m = _relay_re.search(line)
                    if m:
                        _relay_state["total"] += int(m.group(1)) + int(m.group(2))
            _relay_state["offset"] = fh.tell()
        return int(_relay_state["total"])
    except Exception:
        return None


def create_app() -> FastAPI:
    app = FastAPI(title="RemoteDesk", version="0.1.0")

    @app.get("/api/health")
    async def health() -> dict[str, Any]:
        return {"ok": True, **registry.stats()}

    @app.get("/api/stats")
    async def stats() -> dict[str, Any]:
        relay = await asyncio.to_thread(_scrape_coturn_bytes)
        nb = _net_bytes()
        return {
            "uptime_sec": int(time.time() - SERVER_START),
            **registry.stats(),
            "net_bytes_total": nb,
            "net_bytes_session": (nb - NET_START) if nb is not None else None,
            "relay_bytes_total": relay,
            "relay_available": relay is not None,
        }

    @app.get("/api/config")
    async def config() -> dict[str, Any]:
        payload: dict[str, Any] = {
            "mode": "server",
            "transport": "p2p",
            "ice_servers": ice_servers_payload(),
            "demo_host": None,
        }
        if demo_host:
            payload["demo_host"] = {
                "device_id": demo_host["device_id"],
                "device_id_display": format_device_id(demo_host["device_id"]),
                "password": demo_host["password"],
            }
        return payload

    @app.websocket("/ws")
    async def websocket_endpoint(ws: WebSocket) -> None:
        await ws.accept()
        try:
            while True:
                message = await ws.receive()
                if message.get("type") == "websocket.disconnect":
                    break
                if "text" in message and message["text"] is not None:
                    await _handle_text(ws, message["text"])
                # Binary media/files are P2P-only; the signaling socket ignores them.
        except WebSocketDisconnect:
            pass
        except Exception:
            logger.exception("websocket error")
        finally:
            await _cleanup(ws)

    if WEB_DIR.exists():
        app.mount("/static", StaticFiles(directory=WEB_DIR), name="static")

        @app.get("/")
        async def index() -> FileResponse:
            return FileResponse(WEB_DIR / "index.html")

        @app.get("/stats")
        async def stats_page() -> FileResponse:
            return FileResponse(WEB_DIR / "stats.html")

    return app


async def _handle_text(ws: WebSocket, raw: str) -> None:
    try:
        msg = decode_json(raw)
    except ValueError:
        await ws.send_text(encode_json({"type": "error", "message": "invalid json"}))
        return

    kind = msg.get("type")
    if kind == "register":
        await _on_register(ws, msg)
    elif kind == "set_password":
        await _on_set_password(ws, msg)
    elif kind == "refresh_password":
        await _on_refresh_password(ws)
    elif kind == "connect":
        await _on_connect(ws, msg)
    elif kind == "auth_result":
        await _on_auth_result(ws, msg)
    elif kind == "hangup":
        await _on_hangup(ws, msg)
    elif kind == "signal":
        await _on_signal(ws, msg, raw)
    elif kind == "ping":
        device = registry.lookup_by_ws(ws)
        if device:
            registry.touch(device.device_id)
        await ws.send_text(encode_json({"type": "pong", "t": msg.get("t")}))
    else:
        await ws.send_text(encode_json({"type": "error", "message": "unknown message"}))


async def _on_signal(ws: WebSocket, msg: dict[str, Any], raw: str) -> None:
    session = registry.session_for_ws(ws)
    if not session or not session.accepted:
        await ws.send_text(encode_json({"type": "error", "message": "no session"}))
        return
    if msg.get("kind") not in SIGNAL_KINDS:
        await ws.send_text(encode_json({"type": "error", "message": "invalid signal"}))
        return
    peer = session.host_ws if ws is session.viewer_ws else session.viewer_ws
    try:
        await peer.send_text(raw)
    except Exception:
        await _end_and_notify(session.session_id, "peer_disconnected")


async def _on_register(ws: WebSocket, msg: dict[str, Any]) -> None:
    if registry.lookup_by_ws(ws):
        await ws.send_text(encode_json({"type": "error", "message": "already registered"}))
        return
    device = registry.register_host(
        ws=ws,
        hostname=str(msg.get("hostname") or ""),
        os_name=str(msg.get("os") or ""),
        preferred_id=msg.get("device_id"),
        temp_password=msg.get("temp_password"),
    )
    await ws.send_text(
        encode_json(
            {
                "type": "registered",
                "device_id": device.device_id,
                "device_id_display": format_device_id(device.device_id),
                "temp_password": device.temp_password,
            }
        )
    )


async def _on_set_password(ws: WebSocket, msg: dict[str, Any]) -> None:
    device = registry.lookup_by_ws(ws)
    if not device:
        await ws.send_text(encode_json({"type": "error", "message": "not a host"}))
        return
    password = str(msg.get("password") or "")
    if len(password) < 4:
        await ws.send_text(encode_json({"type": "error", "message": "password too short"}))
        return
    registry.set_password(device.device_id, password)
    await ws.send_text(
        encode_json({"type": "password", "temp_password": device.temp_password})
    )


async def _on_refresh_password(ws: WebSocket) -> None:
    device = registry.lookup_by_ws(ws)
    if not device:
        await ws.send_text(encode_json({"type": "error", "message": "not a host"}))
        return
    password = registry.refresh_password(device.device_id)
    await ws.send_text(encode_json({"type": "password", "temp_password": password}))


async def _on_connect(ws: WebSocket, msg: dict[str, Any]) -> None:
    device_id = normalize_device_id(str(msg.get("device_id") or ""))
    password = str(msg.get("password") or "")
    viewer_name = str(msg.get("name") or "viewer")
    host = registry.get(device_id)
    if not host:
        await ws.send_text(encode_json({"type": "auth_failed", "message": "device offline"}))
        return
    if host.session_id:
        await ws.send_text(encode_json({"type": "auth_failed", "message": "device busy"}))
        return
    if not password or not constant_time_equals(password, host.temp_password):
        await ws.send_text(encode_json({"type": "auth_failed", "message": "wrong password"}))
        return
    try:
        session = registry.create_session(host, ws, viewer_name)
    except RuntimeError:
        await ws.send_text(encode_json({"type": "auth_failed", "message": "device busy"}))
        return
    registry.accept(session.session_id)
    accepted = {
        "type": "session_start",
        "session_id": session.session_id,
        "host_id": host.device_id,
        "hostname": host.hostname,
        "os": host.os_name,
        "viewer_name": viewer_name,
        "transport": "p2p",
        "ice_servers": ice_servers_payload(),
    }
    await host.ws.send_text(encode_json(accepted))
    await ws.send_text(encode_json(accepted))


async def _on_auth_result(ws: WebSocket, msg: dict[str, Any]) -> None:
    # Host-side confirmation is optional; password is already checked.
    session = registry.get_session(str(msg.get("session_id") or ""))
    if not session or session.host_ws is not ws:
        return
    if msg.get("ok"):
        registry.accept(session.session_id)
    else:
        await _end_and_notify(session.session_id, "rejected")


async def _on_hangup(ws: WebSocket, msg: dict[str, Any]) -> None:
    session = registry.session_for_ws(ws)
    if session:
        await _end_and_notify(session.session_id, str(msg.get("reason") or "hangup"))


async def _end_and_notify(session_id: str, reason: str) -> None:
    session = registry.end_session(session_id)
    if not session:
        return
    payload = encode_json({"type": "session_end", "session_id": session_id, "reason": reason})
    for peer in (session.host_ws, session.viewer_ws):
        try:
            await peer.send_text(payload)
        except Exception:
            pass


async def _cleanup(ws: WebSocket) -> None:
    dropped = registry.unregister(ws)
    for session in dropped:
        payload = encode_json(
            {"type": "session_end", "session_id": session.session_id, "reason": "peer_disconnected"}
        )
        for peer in (session.host_ws, session.viewer_ws):
            if peer is ws:
                continue
            try:
                await peer.send_text(payload)
            except Exception:
                pass


app = create_app()

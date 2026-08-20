"""Signaling server and static web UI. Media is P2P-only and is never relayed."""

from __future__ import annotations

import asyncio
import logging
import os
import re
import sys
import time
import uuid
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from remote.ids import constant_time_equals, format_device_id, normalize_device_id
from remote.p2p import SIGNAL_KINDS, ice_servers_payload
from remote.protocol import decode_json, encode_json
from remote.server import camlink, oss
from remote.server.registry import Registry

logger = logging.getLogger("remotedesk.server")


def web_dir() -> Path:
    meipass = getattr(sys, "_MEIPASS", None)
    if getattr(sys, "frozen", False) and meipass:
        return Path(meipass) / "remote" / "web"
    return Path(__file__).resolve().parent.parent / "web"


WEB_DIR = web_dir()

registry = Registry()
demo_host: dict[str, str] | None = None
SERVER_START = time.time()
CALL_TIMEOUT_SEC = 45
AGENT_TIMEOUT_SEC = 60
AGENT_MAX_CONTENT = 1 << 20
AGENT_OPS = frozenset({"list", "read", "write", "exec"})
_pending_timers: dict[str, asyncio.Task] = {}
# request_id -> (host_id, future)
_agent_pending: dict[str, tuple[str, asyncio.Future]] = {}


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


def desktop_features_enabled() -> bool:
    """Phone-as-camera and virtual devices exist only in the desktop window."""
    return os.environ.get("DUSTX_DESKTOP", "").strip().lower() in {"1", "true", "yes"}


def _require_desktop() -> None:
    if not desktop_features_enabled():
        raise HTTPException(status_code=404, detail="手机摄像头仅在尘埃X桌面程序中可用")


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
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

    @app.get("/api/health")
    async def health() -> dict[str, Any]:
        return {"ok": True, **registry.stats()}

    @app.post("/api/agent")
    async def agent_http(body: dict[str, Any]) -> Any:
        return await _dispatch_agent(body)

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
            "desktop_app": desktop_features_enabled(),
        }
        if demo_host:
            payload["demo_host"] = {
                "device_id": demo_host["device_id"],
                "device_id_display": format_device_id(demo_host["device_id"]),
                "password": demo_host["password"],
            }
        return payload

    @app.get("/api/downloads/{kind}")
    async def official_download(kind: str, redirect: int = 0) -> Any:
        """Issue a short-lived OSS download URL for an official package.

        Official-site JS fetches this as JSON, then the browser downloads from
        OSS. ``?redirect=1`` 302s for no-JS / direct links.
        """
        if kind not in oss.DOWNLOAD_KINDS:
            raise HTTPException(status_code=404, detail="unknown download")
        try:
            payload = await asyncio.to_thread(oss.download_payload, kind)
        except oss.OssError as exc:
            message = str(exc)
            status = 503 if "not configured" in message else 404
            if redirect:
                raise HTTPException(status_code=status, detail=message) from exc
            return JSONResponse({"ok": False, "message": message}, status_code=status)
        if redirect:
            return RedirectResponse(payload["url"], status_code=302)
        return payload

    @app.get("/api/cam")
    async def cam_info(request: Request) -> dict[str, Any]:
        _require_desktop()
        port = request.url.port or (443 if request.url.scheme == "https" else 80)
        payload = camlink.hub.info(port=port)
        payload["ice_servers"] = ice_servers_payload()
        return payload

    @app.post("/api/cam/rotate")
    async def cam_rotate() -> dict[str, Any]:
        _require_desktop()
        token = camlink.hub.rotate_token()
        return {"ok": True, "token": token}

    @app.post("/api/cam/adb")
    async def cam_adb(request: Request) -> dict[str, Any]:
        _require_desktop()
        port = request.url.port or (443 if request.url.scheme == "https" else 80)
        return await camlink.adb_reverse(port)

    @app.post("/api/cam/sink/start")
    async def cam_sink_start(request: Request) -> dict[str, Any]:
        _require_desktop()
        port = request.url.port or (443 if request.url.scheme == "https" else 80)
        from remote.cam_sink import start_background

        return start_background(f"ws://127.0.0.1:{port}/cam/ws", camlink.hub.token)

    @app.post("/api/cam/sink/stop")
    async def cam_sink_stop() -> dict[str, Any]:
        _require_desktop()
        from remote.cam_sink import stop_background

        return stop_background()

    @app.websocket("/cam/ws")
    async def cam_ws(ws: WebSocket) -> None:
        if not desktop_features_enabled():
            raise HTTPException(status_code=404, detail="手机摄像头仅在尘埃X桌面程序中可用")
        await ws.accept()
        attached = False
        try:
            while True:
                raw = await ws.receive_text()
                try:
                    msg = decode_json(raw)
                except ValueError:
                    await ws.send_text(encode_json({"type": "error", "message": "invalid json"}))
                    continue
                kind = msg.get("type")
                if kind == "hello":
                    role = msg.get("role")
                    token = str(msg.get("token") or "")
                    if role not in {"desktop", "phone"}:
                        await ws.send_json({"type": "error", "message": "role 必须是 desktop 或 phone"})
                        continue
                    attached = await camlink.hub.attach(ws, role, token)
                    if not attached:
                        break
                elif kind == "signal":
                    await camlink.hub.relay(ws, msg)
                else:
                    await ws.send_json({"type": "error", "message": "unknown type"})
        except WebSocketDisconnect:
            pass
        except Exception:
            logger.exception("cam websocket error")
        finally:
            if attached:
                await camlink.hub.detach(ws)

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
    elif kind == "agent_result":
        await _on_agent_result(ws, msg)
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


def _session_payload(session: Any, host: Any) -> dict[str, Any]:
    viewer_id = session.viewer_id or ""
    return {
        "type": "session_start",
        "session_id": session.session_id,
        "host_id": host.device_id,
        "host_id_display": format_device_id(host.device_id),
        "hostname": host.hostname,
        "os": host.os_name,
        "viewer_id": viewer_id,
        "viewer_id_display": format_device_id(viewer_id) if viewer_id else "",
        "viewer_name": session.viewer_name,
        "transport": "p2p",
        "ice_servers": ice_servers_payload(),
    }


def _cancel_call_timer(session_id: str) -> None:
    task = _pending_timers.pop(session_id, None)
    if task and not task.done():
        task.cancel()


async def _on_register(ws: WebSocket, msg: dict[str, Any]) -> None:
    if registry.lookup_by_ws(ws):
        await ws.send_text(encode_json({"type": "error", "message": "already registered"}))
        return
    preferred = normalize_device_id(str(msg.get("device_id") or ""))
    if preferred and len(preferred) == 9:
        existing = registry.get(preferred)
        if existing and existing.ws is not ws:
            try:
                await existing.ws.send_text(
                    encode_json({"type": "replaced", "message": "同一识别码已在其他端登录"})
                )
            except Exception:
                pass
            await _cleanup(existing.ws)
            try:
                await existing.ws.close()
            except Exception:
                pass
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
    caller = registry.lookup_by_ws(ws)
    if caller and caller.device_id == host.device_id:
        await ws.send_text(encode_json({"type": "auth_failed", "message": "cannot connect to self"}))
        return
    if host.session_id or registry.session_for_ws(ws):
        await ws.send_text(encode_json({"type": "auth_failed", "message": "device busy"}))
        return
    if not password or not constant_time_equals(password, host.temp_password):
        await ws.send_text(encode_json({"type": "auth_failed", "message": "wrong password"}))
        return
    try:
        session = registry.create_session(
            host, ws, viewer_name, viewer_id=caller.device_id if caller else None
        )
    except RuntimeError:
        await ws.send_text(encode_json({"type": "auth_failed", "message": "device busy"}))
        return
    incoming = {
        "type": "incoming_call",
        "session_id": session.session_id,
        "viewer_id": session.viewer_id or "",
        "viewer_id_display": format_device_id(session.viewer_id) if session.viewer_id else "",
        "viewer_name": session.viewer_name,
        "host_id": host.device_id,
        "host_id_display": format_device_id(host.device_id),
    }
    await host.ws.send_text(encode_json(incoming))
    await ws.send_text(
        encode_json(
            {
                "type": "call_pending",
                "session_id": session.session_id,
                "host_id": host.device_id,
                "host_id_display": format_device_id(host.device_id),
                "hostname": host.hostname,
            }
        )
    )
    _pending_timers[session.session_id] = asyncio.create_task(_call_timeout(session.session_id))


async def _call_timeout(session_id: str) -> None:
    try:
        await asyncio.sleep(CALL_TIMEOUT_SEC)
    except asyncio.CancelledError:
        return
    session = registry.get_session(session_id)
    if session and not session.accepted:
        await _end_and_notify(session_id, "timeout")


async def _on_auth_result(ws: WebSocket, msg: dict[str, Any]) -> None:
    session = registry.get_session(str(msg.get("session_id") or ""))
    if not session or session.host_ws is not ws:
        return
    _cancel_call_timer(session.session_id)
    if msg.get("ok"):
        registry.accept(session.session_id)
        host = registry.get(session.host_id)
        if not host:
            await _end_and_notify(session.session_id, "host_offline")
            return
        payload = encode_json(_session_payload(session, host))
        for peer in (session.host_ws, session.viewer_ws):
            try:
                await peer.send_text(payload)
            except Exception:
                await _end_and_notify(session.session_id, "peer_disconnected")
                return
    else:
        await _end_and_notify(session.session_id, "rejected")


async def _on_hangup(ws: WebSocket, msg: dict[str, Any]) -> None:
    session = registry.session_for_ws(ws)
    if session:
        await _end_and_notify(session.session_id, str(msg.get("reason") or "hangup"))


async def _end_and_notify(session_id: str, reason: str) -> None:
    _cancel_call_timer(session_id)
    session = registry.end_session(session_id)
    if not session:
        return
    payload = encode_json({"type": "session_end", "session_id": session_id, "reason": reason})
    for peer in (session.host_ws, session.viewer_ws):
        try:
            await peer.send_text(payload)
        except Exception:
            pass


async def _fail_agent_pending(host_id: str | None) -> None:
    if not host_id:
        return
    for rid, (hid, fut) in list(_agent_pending.items()):
        if hid != host_id:
            continue
        _agent_pending.pop(rid, None)
        if fut and not fut.done():
            fut.set_result({"ok": False, "error": "host disconnected", "id": rid})


async def _on_agent_result(ws: WebSocket, msg: dict[str, Any]) -> None:
    if not registry.lookup_by_ws(ws):
        return
    rid = str(msg.get("id") or "")
    item = _agent_pending.pop(rid, None)
    if not item:
        return
    _host_id, fut = item
    if fut and not fut.done():
        fut.set_result(msg)


async def _dispatch_agent(body: dict[str, Any]) -> Any:
    device_id = normalize_device_id(str(body.get("device_id") or ""))
    password = str(body.get("password") or "")
    op = str(body.get("op") or "").strip().lower()
    if len(device_id) != 9 or not password:
        return JSONResponse({"ok": False, "error": "device_id and password required"}, status_code=400)
    if op not in AGENT_OPS:
        return JSONResponse({"ok": False, "error": "unknown op (list/read/write/exec)"}, status_code=400)
    content = str(body.get("content") or "")
    if len(content.encode("utf-8")) > AGENT_MAX_CONTENT:
        return JSONResponse({"ok": False, "error": "content too large"}, status_code=413)
    host = registry.get(device_id)
    if not host:
        return JSONResponse({"ok": False, "error": "device offline"}, status_code=404)
    if not constant_time_equals(password, host.temp_password):
        return JSONResponse({"ok": False, "error": "wrong password"}, status_code=403)
    rid = uuid.uuid4().hex
    loop = asyncio.get_running_loop()
    fut: asyncio.Future = loop.create_future()
    _agent_pending[rid] = (host.device_id, fut)
    payload = {
        "type": "agent",
        "id": rid,
        "op": op,
        "path": str(body.get("path") or ""),
        "content": content,
        "command": str(body.get("command") or ""),
        "cwd": str(body.get("cwd") or ""),
    }
    try:
        await host.ws.send_text(encode_json(payload))
    except Exception:
        _agent_pending.pop(rid, None)
        return JSONResponse({"ok": False, "error": "host send failed"}, status_code=502)
    logger.info("agent %s -> %s op=%s", rid[:8], device_id, op)
    try:
        result = await asyncio.wait_for(fut, timeout=AGENT_TIMEOUT_SEC)
    except asyncio.TimeoutError:
        _agent_pending.pop(rid, None)
        return JSONResponse({"ok": False, "error": "timeout"}, status_code=504)
    if not isinstance(result, dict):
        return JSONResponse({"ok": False, "error": "invalid agent_result"}, status_code=502)
    out = {k: v for k, v in result.items() if k not in {"type", "v"}}
    out.setdefault("ok", False)
    return out


async def _cleanup(ws: WebSocket) -> None:
    device = registry.lookup_by_ws(ws)
    host_id = device.device_id if device else None
    dropped = registry.unregister(ws)
    await _fail_agent_pending(host_id)
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

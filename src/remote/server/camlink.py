"""Local phone-as-camera pairing (LAN or USB). Separate from remote-control /ws."""

from __future__ import annotations

import asyncio
import logging
import secrets
import socket
from typing import Any, Literal

from fastapi import WebSocket

logger = logging.getLogger("remotedesk.camlink")

Role = Literal["desktop", "phone"]


def lan_ipv4s() -> list[str]:
    found: set[str] = set()
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.3)
        sock.connect(("1.1.1.1", 80))
        found.add(sock.getsockname()[0])
        sock.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                found.add(ip)
    except OSError:
        pass
    return sorted(found)


class CamHub:
    """One desktop + one phone; token-gated; signals are relayed only."""

    def __init__(self) -> None:
        self.token = f"{secrets.randbelow(1_000_000):06d}"
        self.desktop: WebSocket | None = None
        self.phone: WebSocket | None = None
        self._lock = asyncio.Lock()

    def rotate_token(self) -> str:
        self.token = f"{secrets.randbelow(1_000_000):06d}"
        return self.token

    def info(self, *, port: int) -> dict[str, Any]:
        ips = lan_ipv4s()
        http = [f"http://{ip}:{port}/" for ip in ips]
        ws = [f"ws://{ip}:{port}/cam/ws" for ip in ips]
        pair = [f"dustcam://{ip}:{port}/{self.token}" for ip in ips]
        return {
            "ok": True,
            "token": self.token,
            "port": port,
            "ips": ips,
            "http_urls": http,
            "ws_urls": ws,
            "pair_urls": pair,
            "usb": {
                "loopback_ws": f"ws://127.0.0.1:{port}/cam/ws",
                "pair_url": f"dustcam://127.0.0.1:{port}/{self.token}?usb=1",
                "adb_reverse": f"adb reverse tcp:{port} tcp:{port}",
                "hint": (
                    "USB：手机打开 USB 网络共享后填电脑在共享网里的 IP；"
                    "或点「准备 USB」让本机执行 adb reverse，手机填 127.0.0.1"
                ),
            },
        }

    async def attach(self, ws: WebSocket, role: Role, token: str) -> bool:
        if token != self.token:
            await ws.send_json({"type": "error", "message": "配对码错误"})
            return False
        async with self._lock:
            current = self.desktop if role == "desktop" else self.phone
            if current is not None and current is not ws:
                try:
                    await current.send_json({"type": "replaced"})
                    await current.close()
                except Exception:
                    pass
            if role == "desktop":
                self.desktop = ws
            else:
                self.phone = ws
        await ws.send_json({"type": "hello_ok", "role": role})
        await self._broadcast_ready()
        return True

    async def detach(self, ws: WebSocket) -> None:
        peer: WebSocket | None = None
        async with self._lock:
            if self.desktop is ws:
                self.desktop = None
                peer = self.phone
            elif self.phone is ws:
                self.phone = None
                peer = self.desktop
        if peer is not None:
            try:
                await peer.send_json({"type": "peer_left"})
            except Exception:
                pass

    async def relay(self, src: WebSocket, msg: dict[str, Any]) -> None:
        dest = self.phone if src is self.desktop else self.desktop
        if dest is None:
            await src.send_json({"type": "error", "message": "对方还没连上"})
            return
        try:
            await dest.send_json(msg)
        except Exception:
            logger.warning("camlink relay failed")

    async def _broadcast_ready(self) -> None:
        if self.desktop is None or self.phone is None:
            return
        payload = {"type": "ready"}
        for side in (self.desktop, self.phone):
            try:
                await side.send_json(payload)
            except Exception:
                pass


hub = CamHub()


async def adb_reverse(port: int) -> dict[str, Any]:
    try:
        proc = await asyncio.create_subprocess_exec(
            "adb",
            "reverse",
            f"tcp:{port}",
            f"tcp:{port}",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        return {"ok": False, "message": "本机没有 adb，请安装 Android platform-tools"}
    out, err = await proc.communicate()
    text = (out or b"").decode("utf-8", "replace") + (err or b"").decode("utf-8", "replace")
    ok = proc.returncode == 0
    return {
        "ok": ok,
        "message": text.strip() or ("已转发" if ok else f"adb 退出 {proc.returncode}"),
        "loopback_ws": f"ws://127.0.0.1:{port}/cam/ws",
        "pair_url": f"dustcam://127.0.0.1:{port}/{hub.token}?usb=1",
    }

"""Host agent: register with signaling, stream frames over WebRTC P2P."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import platform
import sys
from pathlib import Path

from remote.host.capture import now_ms, open_frame_source
from remote.host.files import FileInbox
from remote.host.webrtc import HostPeer
from remote.ids import format_device_id, generate_temp_password
from remote.protocol import decode_json, encode_json, pack_frame, peek_binary_type, unpack_file_chunk, BinaryType

logger = logging.getLogger("remotedesk.host")

CONFIG_DIR = Path.home() / ".remotedesk"
CONFIG_PATH = CONFIG_DIR / "device.json"


def load_device_config() -> dict:
    if CONFIG_PATH.exists():
        try:
            return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {}
    return {}


def save_device_config(data: dict) -> None:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_PATH.write_text(json.dumps(data, indent=2), encoding="utf-8")


def print_banner(device_id: str, password: str, server: str, backend: str) -> None:
    pretty = format_device_id(device_id)
    lines = [
        "",
        "  ┌──────────────────────────────────────────┐",
        "  │            RemoteDesk  被控端            │",
        "  ├──────────────────────────────────────────┤",
        f"  │  本机识别码   {pretty:<26}│",
        f"  │  临时密码     {password:<26}│",
        f"  │  信令服务器   {server:<26}│",
        f"  │  画面来源     {backend:<26}│",
        "  │  状态         在线                       │",
        "  └──────────────────────────────────────────┘",
        "  把识别码和密码发给对方，即可被远程协助。",
        "  仅在你主动运行本程序时才会在线。",
        "",
    ]
    print("\n".join(lines), flush=True)


class HostAgent:
    def __init__(
        self,
        server: str,
        fps: int = 12,
        quality: int = 70,
        prefer_virtual: bool = False,
        backend: str = "auto",
        adb_serial: str | None = None,
        on_registered=None,
    ) -> None:
        self.server = server
        self.fps = max(2, min(fps, 30))
        self.quality = quality
        self.prefer_virtual = prefer_virtual
        self.on_registered = on_registered
        self.source = open_frame_source(
            prefer_virtual=prefer_virtual, backend=backend, adb_serial=adb_serial
        )
        self.inbox = FileInbox()
        self.device_id: str | None = None
        self.password: str | None = None
        self.session_id: str | None = None
        self._stop = asyncio.Event()
        self._ws = None
        self._send_lock = asyncio.Lock()
        self._peer: HostPeer | None = None

    async def run_forever(self) -> None:
        while not self._stop.is_set():
            try:
                await self._run_once()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                logger.warning("host disconnected: %s", exc)
            if not self._stop.is_set():
                await asyncio.sleep(2)

    def stop(self) -> None:
        self._stop.set()

    async def _run_once(self) -> None:
        import websockets

        cfg = load_device_config()
        password = cfg.get("temp_password") or generate_temp_password()
        self.session_id = None
        async with websockets.connect(self.server, max_size=8 * 1024 * 1024) as ws:
            self._ws = ws
            await self._send(
                ws,
                encode_json(
                    {
                        "type": "register",
                        "role": "host",
                        "hostname": platform.node(),
                        "os": f"{platform.system()} {platform.release()}",
                        "device_id": cfg.get("device_id"),
                        "temp_password": password,
                    }
                ),
            )
            raw = await ws.recv()
            msg = decode_json(raw if isinstance(raw, str) else raw.decode())
            if msg.get("type") != "registered":
                raise RuntimeError(f"register failed: {msg}")
            self.device_id = msg["device_id"]
            self.password = msg["temp_password"]
            save_device_config({"device_id": self.device_id, "temp_password": self.password})
            print_banner(self.device_id, self.password, self.server, self.source.backend_name())
            if self.on_registered:
                self.on_registered(self.device_id, self.password)

            tasks = [
                asyncio.create_task(self._consume(ws), name="consume"),
                asyncio.create_task(self._produce(ws), name="produce"),
                asyncio.create_task(self._heartbeat(ws), name="heartbeat"),
                asyncio.create_task(self._stop.wait(), name="stopper"),
            ]
            done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            if self._stop.is_set():
                return
            for task in done:
                exc = task.exception() if not task.cancelled() else None
                if exc:
                    raise exc
            raise ConnectionError("host websocket closed")

    async def _send(self, ws, data) -> None:
        async with self._send_lock:
            await ws.send(data)

    async def _heartbeat(self, ws) -> None:
        while True:
            await asyncio.sleep(15)
            await self._send(ws, encode_json({"type": "ping", "t": now_ms()}))

    async def _produce(self, ws) -> None:
        interval = 1 / self.fps
        while True:
            peer = self._peer
            if peer and peer.is_open:
                jpeg = await asyncio.to_thread(self.source.grab_jpeg, self.quality)
                payload = pack_frame(jpeg, self.source.width, self.source.height, now_ms())
                peer.send(payload)
            await asyncio.sleep(interval)

    async def _consume(self, ws) -> None:
        async for raw in ws:
            if isinstance(raw, bytes):
                self._on_binary(raw)
                continue
            msg = decode_json(raw)
            kind = msg.get("type")
            if kind == "session_start":
                await self._start_peer(ws, msg)
            elif kind == "session_end":
                await self._stop_peer(str(msg.get("reason") or ""))
            elif kind == "signal":
                if self._peer:
                    await self._peer.handle_signal(msg)
            elif kind == "password":
                self.password = msg.get("temp_password")
                if self.device_id and self.password:
                    save_device_config({"device_id": self.device_id, "temp_password": self.password})
                    print_banner(self.device_id, self.password, self.server, self.source.backend_name())

    async def _start_peer(self, ws, msg: dict) -> None:
        if self._peer:
            await self._peer.close()
        self.session_id = msg.get("session_id")
        logger.info("session started by %s (P2P only)", msg.get("viewer_name"))

        async def send_signal(payload: dict) -> None:
            await self._send(ws, encode_json(payload))

        self._peer = HostPeer(send_signal, self._on_session_payload, on_open=self._on_p2p_open)

    def _on_p2p_open(self) -> None:
        if not self._peer:
            return
        self._peer.send(
            encode_json(
                {
                    "type": "screen_info",
                    "width": self.source.width,
                    "height": self.source.height,
                    "backend": self.source.backend_name(),
                }
            )
        )

    async def _stop_peer(self, reason: str) -> None:
        self.session_id = None
        if self._peer:
            await self._peer.close()
            self._peer = None
        logger.info("session ended: %s", reason)

    def _on_session_payload(self, raw: str | bytes) -> None:
        if isinstance(raw, bytes):
            self._on_binary(raw)
            return
        try:
            msg = decode_json(raw)
        except ValueError:
            return
        kind = msg.get("type")
        if kind == "input":
            self._on_input(msg)
        elif kind == "file_offer":
            path = self.inbox.begin(int(msg["id"]), str(msg["name"]), int(msg.get("size") or 0))
            logger.info("receiving file %s -> %s", msg["name"], path)
        elif kind == "file_done":
            path = self.inbox.finish(int(msg["id"]))
            logger.info("file saved: %s", path)
        elif kind == "conn_info":
            method = "TURN 中继（经服务器转发）" if msg.get("method") == "relay" else "P2P 直连"
            print(f"  [连接方式] {method}", flush=True)
            logger.info("connection method: %s", msg.get("method"))
        elif kind == "chat":
            print(f"  [聊天] {msg.get('from', 'viewer')}: {msg.get('text')}", flush=True)
        elif kind == "ping":
            if self._peer:
                self._peer.send(encode_json({"type": "pong", "t": msg.get("t")}))

    def _on_binary(self, data: bytes) -> None:
        try:
            kind = peek_binary_type(data)
        except ValueError:
            return
        if kind == BinaryType.FILE_CHUNK:
            transfer_id, offset, payload = unpack_file_chunk(data)
            self.inbox.write(transfer_id, offset, payload)

    def _on_input(self, msg: dict) -> None:
        event = str(msg.get("event") or "")
        if event in {"move", "down", "up", "scroll"}:
            self.source.handle_mouse(event, int(msg.get("x") or 0), int(msg.get("y") or 0), str(msg.get("button") or "left"))
        elif event in {"keydown", "keyup"}:
            self.source.handle_key("down" if event == "keydown" else "up", str(msg.get("key") or ""))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="RemoteDesk host agent")
    parser.add_argument("--server", default=os.environ.get("REMOTEDESK_SERVER", "ws://127.0.0.1:8080/ws"))
    parser.add_argument("--fps", type=int, default=12)
    parser.add_argument("--quality", type=int, default=70)
    parser.add_argument(
        "--backend",
        choices=["auto", "desktop", "android", "virtual"],
        default="auto",
        help="controlled-side backend (auto picks desktop when a display exists)",
    )
    parser.add_argument("--adb-serial", default=None, help="target Android device serial for --backend android")
    parser.add_argument("--virtual", action="store_true", help="alias for --backend virtual")
    return parser


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    backend = "virtual" if args.virtual else args.backend
    agent = HostAgent(
        server=args.server,
        fps=args.fps,
        quality=args.quality,
        prefer_virtual=args.virtual,
        backend=backend,
        adb_serial=args.adb_serial,
    )
    try:
        asyncio.run(agent.run_forever())
    except KeyboardInterrupt:
        print("\n已退出", file=sys.stderr)


if __name__ == "__main__":
    main()

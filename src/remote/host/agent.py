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
from remote.protocol import decode_json, encode_json, peek_binary_type, unpack_file_chunk, BinaryType

logger = logging.getLogger("remotedesk.host")

CONFIG_DIR = Path.home() / ".remotedesk"
CONFIG_PATH = CONFIG_DIR / "device.json"
TRAFFIC_PATH = CONFIG_DIR / "traffic.json"


def _fmt_bytes(n: float) -> str:
    if n < 1024:
        return f"{int(n)} B"
    if n < 1024 * 1024:
        return f"{n / 1024:.1f} KB"
    if n < 1024 * 1024 * 1024:
        return f"{n / 1024 / 1024:.1f} MB"
    return f"{n / 1024 / 1024 / 1024:.2f} GB"


def _fmt_speed(bytes_per_sec: float) -> str:
    bits = bytes_per_sec * 8
    return f"{bits / 1e3:.0f} Kbps" if bits < 1e6 else f"{bits / 1e6:.2f} Mbps"


def load_traffic_total() -> int:
    try:
        return int(json.loads(TRAFFIC_PATH.read_text(encoding="utf-8")).get("total_bytes", 0))
    except Exception:
        return 0


def save_traffic_total(total: int) -> None:
    try:
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        TRAFFIC_PATH.write_text(json.dumps({"total_bytes": int(total)}), encoding="utf-8")
    except Exception:
        pass


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
        fps: int = 30,
        quality: int = 70,
        prefer_virtual: bool = False,
        backend: str = "auto",
        adb_serial: str | None = None,
        on_registered=None,
        auto_accept: bool = False,
    ) -> None:
        self.server = server
        self.fps = max(2, min(fps, 30))
        self.quality = quality
        self.prefer_virtual = prefer_virtual
        self.on_registered = on_registered
        self.auto_accept = auto_accept
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
        self._traffic_total = load_traffic_total()
        self._rtt: int | None = None
        self._relay_path = False

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
                asyncio.create_task(self._heartbeat(ws), name="heartbeat"),
                asyncio.create_task(self._stats_loop(), name="stats"),
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

    async def _stats_loop(self) -> None:
        import time

        last_bytes = 0
        last_t = time.time()
        while True:
            await asyncio.sleep(2)
            peer = self._peer
            if not peer or not peer.pc:
                last_bytes = 0
                continue
            # measure our own latency: ping the viewer over the data channel
            if peer.is_open:
                peer.send(encode_json({"type": "ping", "t": now_ms()}))
            try:
                report = await peer.pc.getStats()
            except Exception:
                continue
            total = 0
            for stat in report.values():
                if getattr(stat, "type", None) in ("outbound-rtp", "data-channel"):
                    total += getattr(stat, "bytesSent", 0) or 0
            encode = None
            proto = None
            path = None
            for stat in report.values():
                kind = getattr(stat, "type", None)
                mime = str(getattr(stat, "mimeType", "") or "")
                if kind == "codec" and mime.lower().startswith("video/"):
                    encode = mime.split("/")[-1].upper()
                if kind == "candidate-pair" and (
                    getattr(stat, "nominated", False) or getattr(stat, "selected", False)
                ):
                    proto = str(getattr(stat, "protocol", "") or getattr(stat, "networkType", "") or "").upper()
                    ctype = str(getattr(stat, "localCandidateType", "") or "")
                    if "relay" in ctype:
                        path = "TURN"
            now = time.time()
            if last_bytes and total >= last_bytes:
                delta = total - last_bytes
                dt = now - last_t
                self._traffic_total += delta
                save_traffic_total(self._traffic_total)
                speed = delta / dt if dt > 0 else 0
                rtt = f"{self._rtt} ms" if self._rtt is not None else "-- ms"
                extra = ""
                if encode:
                    extra += f" · 编码 {encode}"
                if proto:
                    extra += f" · {proto}"
                if path:
                    extra += f" · {path}"
                print(
                    f"  [流量] 上行 {_fmt_speed(speed)} · 本次 {_fmt_bytes(total)} · 历史 {_fmt_bytes(self._traffic_total)} · 延迟 {rtt}{extra}",
                    flush=True,
                )
            last_bytes = total
            last_t = now

    async def _consume(self, ws) -> None:
        async for raw in ws:
            if isinstance(raw, bytes):
                self._on_binary(raw)
                continue
            msg = decode_json(raw)
            kind = msg.get("type")
            if kind == "incoming_call":
                await self._on_incoming(ws, msg)
            elif kind == "session_start":
                peer = msg.get("viewer_id_display") or msg.get("viewer_id") or msg.get("viewer_name")
                print(f"  [会话] 对方远程码 {peer}", flush=True)
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

    async def _on_incoming(self, ws, msg: dict) -> None:
        peer = msg.get("viewer_id_display") or msg.get("viewer_id") or msg.get("viewer_name") or "?"
        print(f"  [来电] 对方远程码 {peer}（{msg.get('viewer_name') or 'viewer'}）", flush=True)
        ok = self.auto_accept
        if not ok:
            if sys.stdin.isatty():
                try:
                    line = await asyncio.wait_for(
                        asyncio.to_thread(input, "  同意这次远程协助？[y/N] "),
                        timeout=45,
                    )
                    ok = line.strip().lower() in {"y", "yes", "是"}
                except asyncio.TimeoutError:
                    print("  [来电] 超时未同意", flush=True)
                    ok = False
            else:
                ok = True
                print("  [来电] 无交互终端，已自动同意", flush=True)
        await self._send(
            ws,
            encode_json({"type": "auth_result", "session_id": msg.get("session_id"), "ok": ok}),
        )
        if not ok:
            print("  [来电] 已拒绝", flush=True)

    async def _start_peer(self, ws, msg: dict) -> None:
        if self._peer:
            await self._peer.close()
        self.session_id = msg.get("session_id")
        self._relay_path = False
        logger.info("session started by %s (P2P only)", msg.get("viewer_name"))

        async def send_signal(payload: dict) -> None:
            await self._send(ws, encode_json(payload))

        self._peer = HostPeer(
            send_signal,
            self._on_session_payload,
            on_open=self._on_p2p_open,
            source=self.source,
            fps=self.fps,
        )

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
        elif kind == "nav":
            # System navigation (back/home/recents/notifications) — applied on
            # Android hosts via accessibility global actions; desktop just logs it.
            print(f"  [导航] {msg.get('action')}", flush=True)
            logger.info("nav action: %s", msg.get("action"))
        elif kind == "conn_info":
            relay = msg.get("method") == "relay"
            self._relay_path = relay
            method = "TURN 中继（经服务器转发）" if relay else "P2P 直连"
            print(f"  [连接方式] {method}", flush=True)
            logger.info("connection method: %s", msg.get("method"))
            if self._peer:
                asyncio.create_task(self._peer.apply_bitrate(relay=relay))
        elif kind == "qos":
            buf = msg.get("buffer_ms")
            action = msg.get("action")
            print(f"  [QoS] 缓冲 {buf} ms · {action}", flush=True)
            logger.info("qos buffer=%s action=%s", buf, action)
            if self._peer:
                asyncio.create_task(self._peer.apply_bitrate(relay=self._relay_path, stressed=True))
        elif kind == "chat":
            print(f"  [聊天] {msg.get('from', 'viewer')}: {msg.get('text')}", flush=True)
        elif kind == "ping":
            if self._peer:
                self._peer.send(encode_json({"type": "pong", "t": msg.get("t")}))
        elif kind == "pong":
            t = msg.get("t")
            if isinstance(t, (int, float)):
                self._rtt = max(0, now_ms() - int(t))

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
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--quality", type=int, default=70)
    parser.add_argument(
        "--backend",
        choices=["auto", "desktop", "android", "virtual"],
        default="auto",
        help="controlled-side backend (auto picks desktop when a display exists)",
    )
    parser.add_argument("--adb-serial", default=None, help="target Android device serial for --backend android")
    parser.add_argument("--virtual", action="store_true", help="alias for --backend virtual")
    parser.add_argument(
        "--auto-accept",
        action="store_true",
        help="skip the incoming-call prompt (demo / unattended hosts)",
    )
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
        auto_accept=args.auto_accept,
    )
    try:
        asyncio.run(agent.run_forever())
    except KeyboardInterrupt:
        print("\n已退出", file=sys.stderr)


if __name__ == "__main__":
    main()

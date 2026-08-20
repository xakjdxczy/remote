"""Headless mesh viewer: signaling + WebRTC DataChannel + local TCP proxy.

Same hole-punch path as the desktop「跨网互访」tab. Viewer name must contain
``mesh`` so the peer auto-accepts (default ``尘埃X-mesh``).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import struct
import sys
import urllib.request
from typing import Any

from remote.ids import normalize_device_id
from remote.p2p import maybe_strip_relay, rtc_configuration, wait_ice_complete
from remote.protocol import decode_json, encode_json
from remote.urls import official_ws, ws_to_http

logger = logging.getLogger("remotedesk.mesh")

MESH_VIEWER = "尘埃X-mesh"
TYPE_OPEN = 1
TYPE_DATA = 2
TYPE_CLOSE = 3
TYPE_TUN = 4
FRAME = struct.Struct("!BII")


def pack_frame(typ: int, stream_id: int, payload: bytes = b"") -> bytes:
    return FRAME.pack(typ, stream_id & 0xFFFFFFFF, len(payload)) + payload


def parse_frames(data: bytes) -> list[tuple[int, int, bytes]]:
    out: list[tuple[int, int, bytes]] = []
    i = 0
    while i + FRAME.size <= len(data):
        typ, stream_id, n = FRAME.unpack_from(data, i)
        i += FRAME.size
        if n > len(data) - i:
            break
        out.append((typ, stream_id, data[i : i + n]))
        i += n
    return out


def _fetch_json(url: str) -> dict[str, Any]:
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def rtc_config_from_ice(ice_servers: list[dict[str, Any]] | None):
    from aiortc import RTCConfiguration, RTCIceServer

    if not ice_servers:
        return rtc_configuration()
    servers = []
    for item in ice_servers:
        urls = item.get("urls")
        if isinstance(urls, str):
            urls = [urls]
        if not urls:
            continue
        servers.append(
            RTCIceServer(
                urls=list(urls),
                username=item.get("username"),
                credential=item.get("credential"),
            )
        )
    return RTCConfiguration(iceServers=servers) if servers else rtc_configuration()


def ice_from_json(payload: Any):
    from aiortc.sdp import candidate_from_sdp

    if not isinstance(payload, dict):
        return None
    raw = str(payload.get("candidate") or "").strip()
    if not raw:
        return None
    if raw.startswith("candidate:"):
        raw = raw[len("candidate:") :]
    cand = candidate_from_sdp(raw)
    mid = payload.get("sdpMid")
    idx = payload.get("sdpMLineIndex")
    cand.sdpMid = str(mid) if mid is not None else "0"
    if idx is not None:
        cand.sdpMLineIndex = int(idx)
    elif cand.sdpMid is None:
        cand.sdpMLineIndex = 0
    return cand


class MeshViewer:
    def __init__(
        self,
        server: str,
        device_id: str,
        password: str,
        *,
        listen: int = 2222,
        service_port: int = 22,
        name: str = MESH_VIEWER,
        bind: str = "127.0.0.1",
    ) -> None:
        self.server = server
        self.device_id = normalize_device_id(device_id)
        self.password = password
        self.listen = listen
        self.service_port = service_port
        self.name = name
        self.bind = bind
        self._ws = None
        self._pc = None
        self._dc = None
        self._ice = None
        self._next_id = 1
        self._streams: dict[int, tuple[asyncio.StreamReader, asyncio.StreamWriter]] = {}
        self._dc_open = asyncio.Event()
        self._session = asyncio.Event()
        self._outcome = asyncio.Event()
        self._ended = asyncio.Event()
        self._fail: str | None = None
        self._tcp = None

    async def _send(self, payload: dict[str, Any]) -> None:
        if self._ws is None:
            return
        await self._ws.send(encode_json(payload))

    async def _load_ice(self) -> None:
        url = ws_to_http(self.server) + "/api/config"
        try:
            data = await asyncio.to_thread(_fetch_json, url)
            servers = data.get("ice_servers") if isinstance(data, dict) else None
            if isinstance(servers, list) and servers:
                self._ice = rtc_config_from_ice(servers)
                return
        except Exception:
            logger.info("ice config fallback: %s", url)
        self._ice = rtc_configuration()

    async def _start_pc(self) -> None:
        from aiortc import RTCPeerConnection

        if self._pc:
            await self._close_pc()
        pc = RTCPeerConnection(configuration=self._ice or rtc_configuration())
        self._pc = pc
        dc = pc.createDataChannel("mesh", ordered=True)
        self._dc = dc

        @dc.on("open")
        def _on_open() -> None:
            self._dc_open.set()
            print("  [互访] 数据通道已接通，本机监听 "
                  f"{self.bind}:{self.listen} → 对端", flush=True)

        @dc.on("message")
        def _on_message(message: str | bytes) -> None:
            raw = message.encode("utf-8") if isinstance(message, str) else message
            asyncio.create_task(self._on_dc_bytes(raw))

        @pc.on("iceconnectionstatechange")
        def _on_ice() -> None:
            logger.info("ICE %s", pc.iceConnectionState)
            if pc.iceConnectionState == "failed":
                self._fail = "ice failed"
                self._ended.set()
                asyncio.create_task(self._send({"type": "hangup", "reason": "p2p_failed"}))

        offer = await pc.createOffer()
        await pc.setLocalDescription(offer)
        await wait_ice_complete(pc)
        local = pc.localDescription
        await self._send(
            {
                "type": "signal",
                "kind": "offer",
                "sdp": {"type": local.type, "sdp": maybe_strip_relay(local.sdp)},
            }
        )

    async def _handle_signal(self, msg: dict[str, Any]) -> None:
        from aiortc import RTCSessionDescription

        if not self._pc:
            return
        kind = msg.get("kind")
        if kind == "answer":
            sdp = msg.get("sdp") or {}
            await self._pc.setRemoteDescription(
                RTCSessionDescription(sdp=str(sdp.get("sdp") or ""), type=str(sdp.get("type") or "answer"))
            )
        elif kind == "ice":
            cand = ice_from_json(msg.get("candidate"))
            if cand is not None:
                try:
                    await self._pc.addIceCandidate(cand)
                except Exception:
                    logger.debug("addIceCandidate ignored", exc_info=True)
        elif kind == "failed":
            self._fail = str(msg.get("message") or "peer ice failed")

    async def _on_dc_bytes(self, data: bytes) -> None:
        for typ, sid, payload in parse_frames(data):
            if typ == TYPE_DATA:
                pair = self._streams.get(sid)
                if pair:
                    pair[1].write(payload)
                    await pair[1].drain()
            elif typ == TYPE_CLOSE:
                await self._drop_stream(sid, send=False)
            elif typ == TYPE_OPEN:
                asyncio.create_task(self._accept_open(sid))
            elif typ == TYPE_TUN:
                logger.debug("ignore tun frame")

    async def _accept_open(self, sid: int) -> None:
        try:
            reader, writer = await asyncio.open_connection("127.0.0.1", self.service_port)
        except OSError:
            await self._dc_send(pack_frame(TYPE_CLOSE, sid))
            return
        self._streams[sid] = (reader, writer)
        asyncio.create_task(self._pump_local(sid, reader, writer))

    async def _pump_local(self, sid: int, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while True:
                chunk = await reader.read(8192)
                if not chunk:
                    break
                await self._dc_send(pack_frame(TYPE_DATA, sid, chunk))
        finally:
            await self._drop_stream(sid, send=True)

    async def _drop_stream(self, sid: int, *, send: bool) -> None:
        pair = self._streams.pop(sid, None)
        if pair:
            try:
                pair[1].close()
            except Exception:
                pass
        if send:
            await self._dc_send(pack_frame(TYPE_CLOSE, sid))

    async def _dc_send(self, buf: bytes) -> None:
        dc = self._dc
        if not dc or dc.readyState != "open":
            return
        dc.send(buf)

    async def _on_tcp(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        sid = self._next_id
        self._next_id += 1
        self._streams[sid] = (reader, writer)
        await self._dc_send(pack_frame(TYPE_OPEN, sid))
        await self._pump_local(sid, reader, writer)

    async def _close_pc(self) -> None:
        self._dc_open.clear()
        for sid in list(self._streams):
            await self._drop_stream(sid, send=False)
        try:
            if self._dc:
                self._dc.close()
        except Exception:
            pass
        try:
            if self._pc:
                await self._pc.close()
        except Exception:
            pass
        self._dc = None
        self._pc = None

    async def _on_text(self, raw: str) -> None:
        msg = decode_json(raw)
        kind = msg.get("type")
        if kind == "auth_failed":
            text = {
                "device offline": "对端不在线（Windows 尘埃X 互访页需先上线）",
                "wrong password": "密码错误",
                "device busy": "对端正忙（同时只能有一路互访打洞）",
                "cannot connect to self": "不能连自己的识别码",
            }.get(str(msg.get("message") or ""), str(msg.get("message") or "连接失败"))
            self._fail = text
            self._outcome.set()
            print(f"  [互访] {text}", flush=True)
        elif kind == "call_pending":
            print(f"  [互访] 已呼叫 {msg.get('host_id_display') or msg.get('host_id')}，等待同意", flush=True)
        elif kind == "session_start":
            servers = msg.get("ice_servers")
            if isinstance(servers, list) and servers:
                self._ice = rtc_config_from_ice(servers)
            self._session.set()
            self._outcome.set()
            print("  [互访] 对方已同意，正在打洞…", flush=True)
            await self._start_pc()
        elif kind == "signal":
            await self._handle_signal(msg)
        elif kind == "session_end":
            self._fail = str(msg.get("reason") or "session_end")
            self._outcome.set()
            self._ended.set()
            print(f"  [互访] 会话结束：{self._fail}", flush=True)
            if self._tcp:
                self._tcp.close()
            await self._close_pc()
        elif kind == "error":
            print(f"  [互访] {msg.get('message') or '信令错误'}", flush=True)

    async def run(self) -> None:
        import websockets

        await self._load_ice()
        print(f"  [互访] 信令 {self.server}", flush=True)
        print(f"  [互访] 对端 {self.device_id}  本机 {self.bind}:{self.listen}", flush=True)
        async with websockets.connect(self.server, max_size=2**23) as ws:
            self._ws = ws
            await self._send(
                {
                    "type": "connect",
                    "device_id": self.device_id,
                    "password": self.password,
                    "name": self.name,
                }
            )

            async def ping() -> None:
                while True:
                    await asyncio.sleep(20)
                    await self._send({"type": "ping", "t": int(asyncio.get_running_loop().time() * 1000)})

            ping_task = asyncio.create_task(ping())
            consume = asyncio.create_task(self._consume())
            try:
                await asyncio.wait_for(self._outcome.wait(), timeout=50)
                if self._fail or not self._session.is_set():
                    raise RuntimeError(self._fail or "连接失败")
                await asyncio.wait_for(self._dc_open.wait(), timeout=45)
                self._tcp = await asyncio.start_server(self._on_tcp, self.bind, self.listen)
                print(
                    f"  [互访] 已监听。本机执行：ssh -p {self.listen} 对端用户名@{self.bind}",
                    flush=True,
                )
                async with self._tcp:
                    await asyncio.wait(
                        {
                            asyncio.create_task(self._tcp.serve_forever()),
                            asyncio.create_task(self._ended.wait()),
                        },
                        return_when=asyncio.FIRST_COMPLETED,
                    )
            except asyncio.TimeoutError as exc:
                raise RuntimeError(self._fail or "打洞超时（可走 VPS TURN，或改用 python -m remote agent）") from exc
            finally:
                ping_task.cancel()
                consume.cancel()
                if self._tcp:
                    self._tcp.close()
                await self._send({"type": "hangup", "reason": "offline"})
                await self._close_pc()

    async def _consume(self) -> None:
        assert self._ws is not None
        async for raw in self._ws:
            if isinstance(raw, bytes):
                continue
            try:
                await self._on_text(raw)
            except Exception:
                logger.exception("mesh signal")
            if self._fail and not self._session.is_set():
                return


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="尘埃X 跨网互访打洞（WebRTC 隧道，本机端口转发）")
    p.add_argument("--server", default=official_ws(), help="信令 WebSocket，默认官网")
    p.add_argument("--device", required=True, help="对端 9 位识别码")
    p.add_argument("--password", required=True, help="对端互访密码")
    p.add_argument("--listen", type=int, default=2222, help="本机监听端口，默认 2222")
    p.add_argument("--bind", default="127.0.0.1")
    p.add_argument("--service-port", type=int, default=22, help="对端 OPEN 时本机回连端口")
    p.add_argument("--name", default=MESH_VIEWER, help="必须包含 mesh，对端才会自动同意")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    viewer = MeshViewer(
        args.server,
        args.device,
        args.password,
        listen=args.listen,
        service_port=args.service_port,
        name=args.name,
        bind=args.bind,
    )
    try:
        asyncio.run(viewer.run())
    except KeyboardInterrupt:
        print("\n已退出", file=sys.stderr)
        return 0
    except Exception as exc:
        print(f"  [互访] {exc}", file=sys.stderr)
        return 1
    return 0

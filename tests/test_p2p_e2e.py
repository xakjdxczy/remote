import asyncio
import json
import socket
import threading
import time

import pytest
import uvicorn
import websockets

aiortc = pytest.importorskip("aiortc")

from remote.host.agent import HostAgent
from remote.p2p import rtc_configuration, strip_relay_sdp, wait_ice_complete
from remote.protocol import BinaryType, peek_binary_type
from remote.server.api import create_app


def _free_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


async def _viewer_receive_frame(server: str, device_id: str, password: str) -> bytes:
    from aiortc import RTCPeerConnection, RTCSessionDescription

    async with websockets.connect(server, max_size=8 * 1024 * 1024) as ws:
        await ws.send(json.dumps({
            "type": "connect",
            "device_id": device_id,
            "password": password,
            "name": "e2e",
        }))
        start = json.loads(await ws.recv())
        assert start["type"] == "session_start"
        assert start["transport"] == "p2p"

        pc = RTCPeerConnection(configuration=rtc_configuration())
        channel = pc.createDataChannel("session")
        frames: list[bytes] = []
        opened = asyncio.Event()

        @channel.on("open")
        def _on_open() -> None:
            opened.set()

        @channel.on("message")
        def _on_message(message) -> None:
            if isinstance(message, bytes):
                frames.append(message)

        offer = await pc.createOffer()
        await pc.setLocalDescription(offer)
        await wait_ice_complete(pc)
        await ws.send(json.dumps({
            "type": "signal",
            "kind": "offer",
            "sdp": {"type": pc.localDescription.type, "sdp": strip_relay_sdp(pc.localDescription.sdp)},
        }))

        while True:
            raw = await asyncio.wait_for(ws.recv(), timeout=8)
            msg = json.loads(raw)
            if msg.get("type") == "signal" and msg.get("kind") == "answer":
                await pc.setRemoteDescription(
                    RTCSessionDescription(sdp=msg["sdp"]["sdp"], type=msg["sdp"]["type"])
                )
                break

        await asyncio.wait_for(opened.wait(), timeout=8)
        deadline = time.time() + 8
        while time.time() < deadline and not frames:
            await asyncio.sleep(0.05)
        await ws.send(json.dumps({"type": "hangup", "reason": "done"}))
        await pc.close()
        assert frames, "P2P datachannel received no frames"
        return frames[0]


def test_p2p_e2e_frame_over_datachannel():
    port = _free_port()
    config = uvicorn.Config(create_app(), host="127.0.0.1", port=port, log_level="warning")
    server = uvicorn.Server(config)
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    deadline = time.time() + 8
    while not server.started and time.time() < deadline:
        time.sleep(0.05)
    assert server.started

    creds: dict[str, str] = {}

    def on_registered(device_id: str, password: str) -> None:
        creds["device_id"] = device_id
        creds["password"] = password

    agent = HostAgent(
        server=f"ws://127.0.0.1:{port}/ws",
        fps=8,
        prefer_virtual=True,
        on_registered=on_registered,
    )
    host_thread = threading.Thread(target=lambda: asyncio.run(agent.run_forever()), daemon=True)
    host_thread.start()
    wait = time.time() + 8
    while "device_id" not in creds and time.time() < wait:
        time.sleep(0.05)
    assert "device_id" in creds

    frame = asyncio.run(_viewer_receive_frame(f"ws://127.0.0.1:{port}/ws", creds["device_id"], creds["password"]))
    assert peek_binary_type(frame) == BinaryType.FRAME
    assert b"\xff\xd8" in frame
    agent.stop()
    server.should_exit = True

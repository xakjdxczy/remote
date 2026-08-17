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
from remote.server.api import create_app


def _free_port() -> int:
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


async def _viewer_receive_video(server: str, device_id: str, password: str):
    from aiortc import RTCPeerConnection, RTCSessionDescription

    async with websockets.connect(server, max_size=8 * 1024 * 1024) as ws:
        await ws.send(json.dumps({
            "type": "connect",
            "device_id": device_id,
            "password": password,
            "name": "e2e",
        }))
        pending = json.loads(await ws.recv())
        assert pending["type"] == "call_pending"
        start = json.loads(await ws.recv())
        assert start["type"] == "session_start"
        assert start["transport"] == "p2p"

        pc = RTCPeerConnection(configuration=rtc_configuration())
        pc.createDataChannel("session")
        pc.addTransceiver("video", direction="recvonly")
        got = asyncio.Event()
        holder: dict = {}

        @pc.on("track")
        def _on_track(track) -> None:
            async def _reader() -> None:
                try:
                    holder["frame"] = await track.recv()
                    got.set()
                except Exception:
                    pass

            asyncio.ensure_future(_reader())

        offer = await pc.createOffer()
        await pc.setLocalDescription(offer)
        await wait_ice_complete(pc)
        await ws.send(json.dumps({
            "type": "signal",
            "kind": "offer",
            "sdp": {"type": pc.localDescription.type, "sdp": strip_relay_sdp(pc.localDescription.sdp)},
        }))

        while True:
            raw = await asyncio.wait_for(ws.recv(), timeout=10)
            msg = json.loads(raw)
            if msg.get("type") == "signal" and msg.get("kind") == "answer":
                await pc.setRemoteDescription(
                    RTCSessionDescription(sdp=msg["sdp"]["sdp"], type=msg["sdp"]["type"])
                )
                break

        await asyncio.wait_for(got.wait(), timeout=20)
        await ws.send(json.dumps({"type": "hangup", "reason": "done"}))
        frame = holder["frame"]
        await pc.close()
        return frame


def test_p2p_e2e_video_track():
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
        auto_accept=True,
    )
    host_thread = threading.Thread(target=lambda: asyncio.run(agent.run_forever()), daemon=True)
    host_thread.start()
    wait = time.time() + 8
    while "device_id" not in creds and time.time() < wait:
        time.sleep(0.05)
    assert "device_id" in creds

    frame = asyncio.run(_viewer_receive_video(f"ws://127.0.0.1:{port}/ws", creds["device_id"], creds["password"]))
    # A decoded WebRTC video frame from the host's screen track.
    assert frame is not None
    assert getattr(frame, "width", 0) > 0
    assert getattr(frame, "height", 0) > 0
    agent.stop()
    server.should_exit = True

"""Desktop sink: receive phone A/V and feed optional virtual camera/mic."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import threading

from remote.p2p import rtc_configuration
from remote.virtualio import VirtualCamera, VirtualMic

logger = logging.getLogger("remotedesk.cam_sink")

_sink_thread: threading.Thread | None = None
_sink_stop = threading.Event()


def sink_running() -> bool:
    return _sink_thread is not None and _sink_thread.is_alive()


def start_background(url: str, token: str) -> dict[str, object]:
    global _sink_thread
    if sink_running():
        return {"ok": True, "running": True, "message": "虚拟设备输出已在运行"}
    _sink_stop.clear()

    def worker() -> None:
        try:
            asyncio.run(run_sink(url, token, stop=_sink_stop))
        except Exception:
            logger.exception("cam sink stopped with error")

    _sink_thread = threading.Thread(target=worker, name="dustx-cam-sink", daemon=True)
    _sink_thread.start()
    return {"ok": True, "running": True, "message": "已开始输出到系统虚拟摄像头/麦克风"}


def stop_background() -> dict[str, object]:
    _sink_stop.set()
    return {"ok": True, "running": False, "message": "已停止虚拟设备输出"}


async def run_sink(url: str, token: str, stop: threading.Event | None = None) -> None:
    from aiortc import RTCPeerConnection, RTCSessionDescription
    import numpy as np
    from websockets.asyncio.client import connect

    pc = RTCPeerConnection(configuration=rtc_configuration())
    vcam = VirtualCamera()
    vmic = VirtualMic()
    cam_ok = vcam.open()
    mic_ok = vmic.open()
    if not cam_ok:
        logger.warning("未打开虚拟摄像头：可 pip install pyvirtualcam，并先启动 OBS 虚拟摄像头")
    if not mic_ok:
        logger.warning("未打开虚拟麦克风：macOS 安装 BlackHole，Windows 安装 VB-CABLE")

    @pc.on("track")
    async def on_track(track):  # type: ignore[no-untyped-def]
        if track.kind == "video" and cam_ok:
            while True:
                try:
                    frame = await track.recv()
                except Exception:
                    break
                arr = frame.to_ndarray(format="rgb24")
                vcam.send_rgb(arr)
        elif track.kind == "audio" and mic_ok:
            while True:
                try:
                    frame = await track.recv()
                except Exception:
                    break
                data = frame.to_ndarray()
                if data.ndim == 2:
                    pcm = np.mean(data.astype("float32"), axis=0)
                else:
                    pcm = data.astype("float32")
                peak = float(np.max(np.abs(pcm))) if pcm.size else 1.0
                if peak > 1.0:
                    pcm = pcm / peak
                vmic.write(pcm.reshape(-1, 1))

    pc.addTransceiver("video", direction="recvonly")
    pc.addTransceiver("audio", direction="recvonly")

    async with connect(url) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "desktop", "token": token}))
        offer_sent = False
        async for raw in ws:
            if stop is not None and stop.is_set():
                break
            msg = json.loads(raw)
            kind = msg.get("type")
            if kind == "error":
                logger.error("%s", msg.get("message"))
                break
            if kind in {"hello_ok", "ready"} and not offer_sent:
                offer = await pc.createOffer()
                await pc.setLocalDescription(offer)
                await ws.send(
                    json.dumps(
                        {
                            "type": "signal",
                            "kind": "offer",
                            "sdp": {"type": pc.localDescription.type, "sdp": pc.localDescription.sdp},
                        }
                    )
                )
                offer_sent = True
            elif kind == "signal" and msg.get("kind") == "answer":
                sdp = msg.get("sdp") or {}
                await pc.setRemoteDescription(
                    RTCSessionDescription(sdp=sdp.get("sdp", ""), type=sdp.get("type", "answer"))
                )
            elif kind == "peer_left":
                logger.info("phone left")
                break

    vcam.close()
    vmic.close()
    await pc.close()


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Feed phone camera/mic into virtual devices")
    parser.add_argument("--url", default="ws://127.0.0.1:8080/cam/ws")
    parser.add_argument("--token", required=True)
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
    asyncio.run(run_sink(args.url, args.token))

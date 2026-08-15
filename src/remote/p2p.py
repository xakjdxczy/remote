"""WebRTC P2P helpers. Media never uses TURN or the signaling server."""

from __future__ import annotations

import asyncio
from typing import Any

# STUN only helps discover addresses. There is no TURN / media relay.
STUN_URLS = ["stun:stun.l.google.com:19302"]
SIGNAL_KINDS = frozenset({"offer", "answer", "ice", "failed"})


def ice_servers_payload() -> list[dict[str, Any]]:
    return [{"urls": list(STUN_URLS)}]


def rtc_configuration():
    from aiortc import RTCConfiguration, RTCIceServer

    return RTCConfiguration(iceServers=[RTCIceServer(urls=list(STUN_URLS))])


def _is_relay_candidate_line(line: str) -> bool:
    if not line.startswith("a=candidate:"):
        return False
    parts = line.split()
    try:
        typ_at = parts.index("typ")
    except ValueError:
        return False
    return typ_at + 1 < len(parts) and parts[typ_at + 1] == "relay"


def strip_relay_sdp(sdp: str) -> str:
    """Drop any ICE relay candidates so media cannot fall back to TURN."""
    kept = [line for line in sdp.replace("\r\n", "\n").split("\n") if not _is_relay_candidate_line(line)]
    text = "\r\n".join(kept)
    if not text.endswith("\r\n"):
        text += "\r\n"
    return text


async def wait_ice_complete(pc, timeout: float = 8.0) -> None:
    if pc.iceGatheringState == "complete":
        return
    done = asyncio.get_running_loop().create_future()

    @pc.on("icegatheringstatechange")
    def _on_gather() -> None:
        if pc.iceGatheringState == "complete" and not done.done():
            done.set_result(None)

    if pc.iceGatheringState == "complete":
        return
    try:
        await asyncio.wait_for(asyncio.shield(done), timeout=timeout)
    except asyncio.TimeoutError:
        pass

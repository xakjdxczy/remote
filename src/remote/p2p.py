"""WebRTC P2P helpers.

Prefers direct P2P (STUN only). An optional TURN relay can be enabled via
environment variables (``TURN_URLS`` / ``TURN_USER`` / ``TURN_PASS``) so that
sessions still connect on networks where hole-punching fails. When TURN is not
configured the app stays pure P2P and relay candidates are stripped.
"""

from __future__ import annotations

import asyncio
import os
from typing import Any

# STUN only helps discover addresses. Several public STUN servers are listed so
# hosts behind NAT (including in regions where Google's STUN is unreachable) can
# still discover their public server-reflexive candidate. ICE tries them all.
STUN_URLS = [
    "stun:stun.l.google.com:19302",
    "stun:stun.qq.com:3478",
    "stun:stun.miwifi.com:3478",
    "stun:stun.cloudflare.com:3478",
]
SIGNAL_KINDS = frozenset({"offer", "answer", "ice", "failed"})


def turn_config() -> tuple[list[str], str | None, str | None]:
    """Return (turn_urls, username, credential); empty when TURN is disabled."""
    urls = [u.strip() for u in os.environ.get("TURN_URLS", "").split(",") if u.strip()]
    user = os.environ.get("TURN_USER") or None
    cred = os.environ.get("TURN_PASS") or None
    if urls and user and cred:
        return urls, user, cred
    return [], None, None


def relay_enabled() -> bool:
    return bool(turn_config()[0])


def ice_servers_payload() -> list[dict[str, Any]]:
    servers: list[dict[str, Any]] = [{"urls": list(STUN_URLS)}]
    turn_urls, user, cred = turn_config()
    if turn_urls:
        servers.append({"urls": turn_urls, "username": user, "credential": cred})
    return servers


def rtc_configuration():
    from aiortc import RTCConfiguration, RTCIceServer

    ice = [RTCIceServer(urls=list(STUN_URLS))]
    turn_urls, user, cred = turn_config()
    if turn_urls:
        ice.append(RTCIceServer(urls=turn_urls, username=user, credential=cred))
    return RTCConfiguration(iceServers=ice)


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
    """Drop ICE relay candidates (used only when TURN relay is disabled)."""
    kept = [line for line in sdp.replace("\r\n", "\n").split("\n") if not _is_relay_candidate_line(line)]
    text = "\r\n".join(kept)
    if not text.endswith("\r\n"):
        text += "\r\n"
    return text


def maybe_strip_relay(sdp: str) -> str:
    """Strip relay candidates unless TURN relay is enabled via environment."""
    return sdp if relay_enabled() else strip_relay_sdp(sdp)


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

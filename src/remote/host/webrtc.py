"""Host-side WebRTC peer. Session bytes only travel on the data channel."""

from __future__ import annotations

import logging
from collections.abc import Awaitable, Callable
from typing import Any

from remote.p2p import maybe_strip_relay, rtc_configuration, wait_ice_complete

logger = logging.getLogger("remotedesk.host.webrtc")

SendSignal = Callable[[dict[str, Any]], Awaitable[None]]
OnPayload = Callable[[str | bytes], None]
OnOpen = Callable[[], None]


class HostPeer:
    def __init__(self, send_signal: SendSignal, on_payload: OnPayload, on_open: OnOpen | None = None) -> None:
        from aiortc import RTCPeerConnection

        self._send_signal = send_signal
        self._on_payload = on_payload
        self._on_open = on_open
        self.pc = RTCPeerConnection(configuration=rtc_configuration())
        self.channel = None
        self._bind_pc()

    def _bind_pc(self) -> None:
        @self.pc.on("datachannel")
        def _on_datachannel(channel) -> None:
            self._attach_channel(channel)

        @self.pc.on("iceconnectionstatechange")
        def _on_ice() -> None:
            logger.info("ICE %s", self.pc.iceConnectionState)

        @self.pc.on("connectionstatechange")
        def _on_conn() -> None:
            logger.info("P2P %s", self.pc.connectionState)

    def _attach_channel(self, channel) -> None:
        self.channel = channel
        logger.info("P2P datachannel %s", channel.label)

        @channel.on("message")
        def _on_message(message: str | bytes) -> None:
            self._on_payload(message)

        @channel.on("open")
        def _on_open() -> None:
            if self._on_open:
                self._on_open()

        if channel.readyState == "open" and self._on_open:
            self._on_open()

        @self.pc.on("iceconnectionstatechange")
        def _on_ice() -> None:
            logger.info("ICE %s", self.pc.iceConnectionState)

        @self.pc.on("connectionstatechange")
        def _on_conn() -> None:
            logger.info("P2P %s", self.pc.connectionState)

    @property
    def is_open(self) -> bool:
        return bool(self.channel) and self.channel.readyState == "open"

    def send(self, data: str | bytes) -> None:
        if not self.is_open:
            return
        self.channel.send(data)

    async def handle_signal(self, msg: dict[str, Any]) -> None:
        from aiortc import RTCSessionDescription

        kind = msg.get("kind")
        if kind == "offer":
            sdp = msg.get("sdp") or {}
            await self.pc.setRemoteDescription(
                RTCSessionDescription(sdp=str(sdp.get("sdp") or ""), type=str(sdp.get("type") or "offer"))
            )
            answer = await self.pc.createAnswer()
            await self.pc.setLocalDescription(answer)
            await wait_ice_complete(self.pc)
            local = self.pc.localDescription
            await self._send_signal(
                {
                    "type": "signal",
                    "kind": "answer",
                    "sdp": {"type": local.type, "sdp": maybe_strip_relay(local.sdp)},
                }
            )
        elif kind == "failed":
            logger.warning("viewer reported P2P failure: %s", msg.get("message"))

    async def close(self) -> None:
        try:
            if self.channel:
                self.channel.close()
        except Exception:
            pass
        try:
            await self.pc.close()
        except Exception:
            pass
        self.channel = None

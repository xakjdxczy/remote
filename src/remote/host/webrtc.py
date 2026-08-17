"""Host-side WebRTC peer.

The screen is sent as a real WebRTC **video track** (VP8/H264, encoded by
aiortc) instead of JPEG-over-datachannel: far less bandwidth (inter-frame
compression, near-zero when the screen is static) and adaptive bitrate. Input,
chat, files and pings still travel on the "session" data channel.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable
from fractions import Fraction
from typing import Any

from aiortc import VideoStreamTrack

from remote.latency import (
    KEYFRAME_BUDGET_MS,
    LUMA_SAMPLE_H,
    LUMA_SAMPLE_W,
    apply_video_bitrate_fmtp,
    catchup_stale_count,
    is_scene_change,
    keyframe_budget_bytes,
    scene_burst_ladder,
    scene_luma_diff,
    video_bitrate_kbps,
)
from remote.p2p import maybe_strip_relay, rtc_configuration, wait_ice_complete

logger = logging.getLogger("remotedesk.host.webrtc")

SendSignal = Callable[[dict[str, Any]], Awaitable[None]]
OnPayload = Callable[[str | bytes], None]
OnOpen = Callable[[], None]

VIDEO_CLOCK_RATE = 90000


def _sample_luma(img: Any, sw: int = LUMA_SAMPLE_W, sh: int = LUMA_SAMPLE_H) -> list[int]:
    small = img.resize((sw, sh))
    if getattr(small, "mode", "RGB") != "L":
        small = small.convert("L")
    flat = getattr(small, "get_flattened_data", None)
    if callable(flat):
        return list(flat())
    return list(small.getdata())


class ScreenVideoTrack(VideoStreamTrack):
    """A WebRTC video track that pulls frames from a FrameSource at ~fps.

    On a desktop/app switch the capturer is often late and the encoder wants a
    giant intra. This track drops stale ticks (only the latest frame is sent)
    and temporarily downscales so that I-frame fits the 80–120ms send budget.
    """

    kind = "video"

    def __init__(
        self,
        source: Any,
        fps: int = 12,
        *,
        bitrate_bps: int | None = None,
        on_scene: Callable[[], None] | None = None,
    ) -> None:
        super().__init__()
        self._source = source
        self._fps = max(2, min(fps, 30))
        self._interval = 1.0 / self._fps
        self._start: float | None = None
        self._count = 0
        self._bitrate_bps = int(bitrate_bps or video_bitrate_kbps(relay=False)[2] * 1000)
        self._on_scene = on_scene
        self._prev_luma: list[int] | None = None
        self._full_w = 0
        self._full_h = 0
        self._ladder: list[tuple[int, int, int]] = []
        self._ladder_i = 0
        self._ladder_until = 0.0
        self._force_i = False

    def set_bitrate(self, bitrate_bps: int) -> None:
        self._bitrate_bps = max(1, int(bitrate_bps))

    def force_burst(self) -> None:
        w = self._full_w or int(getattr(self._source, "width", 0) or 1280)
        h = self._full_h or int(getattr(self._source, "height", 0) or 720)
        self._begin_burst(w, h)

    def _begin_burst(self, width: int, height: int) -> None:
        budget = keyframe_budget_bytes(self._bitrate_bps, KEYFRAME_BUDGET_MS)
        self._ladder = scene_burst_ladder(width, height, budget)
        self._ladder_i = 0
        hold = self._ladder[0][2] if self._ladder else 0
        self._ladder_until = time.time() + hold / 1000.0
        self._force_i = True
        if self._on_scene:
            try:
                self._on_scene()
            except Exception:
                logger.debug("on_scene callback failed", exc_info=True)

    def _step_size(self, width: int, height: int) -> tuple[int, int]:
        if not self._ladder:
            return width, height
        now = time.time()
        while self._ladder_i < len(self._ladder) - 1 and now >= self._ladder_until:
            self._ladder_i += 1
            hold = self._ladder[self._ladder_i][2]
            self._ladder_until = now + hold / 1000.0
            self._force_i = True
        if self._ladder_i >= len(self._ladder) - 1:
            w, h, _ = self._ladder[-1]
            self._ladder = []
            return w, h
        return self._ladder[self._ladder_i][0], self._ladder[self._ladder_i][1]

    async def recv(self):
        import av  # bundled with aiortc

        now = time.time()
        if self._start is None:
            self._start = now
        else:
            self._count = catchup_stale_count(now, self._start, self._count, self._interval)
        target = self._start + self._count * self._interval
        delay = target - time.time()
        if delay > 0:
            await asyncio.sleep(delay)
        img = await asyncio.to_thread(self._source.grab_image)
        width, height = img.size
        self._full_w, self._full_h = width, height
        luma = await asyncio.to_thread(_sample_luma, img)
        if self._prev_luma is not None and not self._ladder:
            if is_scene_change(scene_luma_diff(self._prev_luma, luma)):
                self._begin_burst(width, height)
        self._prev_luma = luma
        out_w, out_h = self._step_size(width, height)
        if (out_w, out_h) != (width, height):
            img = await asyncio.to_thread(img.resize, (out_w, out_h))
        frame = av.VideoFrame.from_image(img)
        if self._force_i:
            try:
                frame.pict_type = "I"
            except Exception:
                pass
            self._force_i = False
        frame.pts = int(self._count * (VIDEO_CLOCK_RATE / self._fps))
        frame.time_base = Fraction(1, VIDEO_CLOCK_RATE)
        self._count += 1
        return frame


class HostPeer:
    def __init__(
        self,
        send_signal: SendSignal,
        on_payload: OnPayload,
        on_open: OnOpen | None = None,
        source: Any | None = None,
        fps: int = 12,
    ) -> None:
        from aiortc import RTCPeerConnection

        self._send_signal = send_signal
        self._on_payload = on_payload
        self._on_open = on_open
        self._source = source
        self._fps = fps
        self.pc = RTCPeerConnection(configuration=rtc_configuration())
        self.channel = None
        self._track: ScreenVideoTrack | None = None
        self._bind_pc()

    def _emit_scene_change(self) -> None:
        if self.is_open:
            self.send('{"type":"scene_change"}')

    def force_scene_burst(self) -> None:
        if self._track:
            self._track.force_burst()
        else:
            self._emit_scene_change()

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

    @property
    def is_open(self) -> bool:
        return bool(self.channel) and self.channel.readyState == "open"

    def send(self, data: str | bytes) -> None:
        if not self.is_open:
            return
        self.channel.send(data)

    async def apply_bitrate(self, *, relay: bool, stressed: bool = False) -> None:
        """Cap the outbound video encoder when the peer API supports it."""
        _min_k, _start_k, max_k = video_bitrate_kbps(relay=relay, stressed=stressed)
        max_bps = max_k * 1000
        for sender in self.pc.getSenders():
            track = getattr(sender, "track", None)
            if not track or getattr(track, "kind", None) != "video":
                continue
            try:
                params = sender.getParameters()
            except Exception:
                continue
            encodings = getattr(params, "encodings", None) or []
            if not encodings:
                continue
            try:
                encodings[0].maxBitrate = max_bps
                await sender.setParameters(params)
                logger.info("video maxBitrate=%s (relay=%s stressed=%s)", max_bps, relay, stressed)
            except Exception as exc:
                logger.info("setParameters skipped: %s", exc)
        if self._track:
            self._track.set_bitrate(max_bps)

    async def handle_signal(self, msg: dict[str, Any]) -> None:
        from aiortc import RTCSessionDescription

        kind = msg.get("kind")
        if kind == "offer":
            sdp = msg.get("sdp") or {}
            await self.pc.setRemoteDescription(
                RTCSessionDescription(sdp=str(sdp.get("sdp") or ""), type=str(sdp.get("type") or "offer"))
            )
            # Attach the screen video track to answer the viewer's recvonly video.
            if self._source is not None and self._track is None:
                self._track = ScreenVideoTrack(
                    self._source,
                    self._fps,
                    on_scene=self._emit_scene_change,
                )
                self.pc.addTrack(self._track)
            await self.apply_bitrate(relay=False)
            answer = await self.pc.createAnswer()
            await self.pc.setLocalDescription(answer)
            await wait_ice_complete(self.pc)
            local = self.pc.localDescription
            min_k, start_k, max_k = video_bitrate_kbps(relay=False)
            sdp = apply_video_bitrate_fmtp(maybe_strip_relay(local.sdp), min_k, start_k, max_k)
            await self._send_signal(
                {
                    "type": "signal",
                    "kind": "answer",
                    "sdp": {"type": local.type, "sdp": sdp},
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
            if self._track:
                self._track.stop()
        except Exception:
            pass
        try:
            await self.pc.close()
        except Exception:
            pass
        self.channel = None
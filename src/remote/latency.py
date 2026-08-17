"""Shared low-latency policy for RemoteDesk video.

The viewer jitter-buffer number is Chrome holding frames to absorb packet
jitter. On TURN that jitter is larger, and a 3 Mbps burst (the previous
uncapped Android uplink) queues at the relay so the buffer grows to 100ms+.

These helpers keep bitrate / catch-up / SDP hints consistent across the
Python host, tests, and the documented viewer/Android constants.
"""

from __future__ import annotations

# One frame at 30 fps is ~33ms. Chrome treats jitterBufferTarget=0 as "unset"
# on some versions and then uses a ~100ms default; a small positive target
# actually requests the low-latency path.
JITTER_BUFFER_TARGET_MS = 16

# Viewer catch-up: slightly faster playback drains a swollen buffer.
# (May be ignored for live MediaStream in some browsers; still cheap to apply.)
BUFFER_CATCHUP_SOFT_MS = 70
BUFFER_CATCHUP_HARD_MS = 120
BUFFER_KEYFRAME_MS = 180


def playback_rate_for_buffer_ms(jb_ms: float) -> float:
    """Return a playbackRate that drains `jb_ms` of jitter-buffer delay."""
    if jb_ms >= 200:
        return 1.35
    if jb_ms >= BUFFER_CATCHUP_HARD_MS:
        return 1.18
    if jb_ms >= BUFFER_CATCHUP_SOFT_MS:
        return 1.08
    return 1.0


def should_request_keyframe(jb_ms: float) -> bool:
    """True when the receiver should ask the sender for a fresh keyframe."""
    return jb_ms >= BUFFER_KEYFRAME_MS


def video_bitrate_kbps(*, relay: bool, stressed: bool = False) -> tuple[int, int, int]:
    """Return ``(min, start, max)`` encoder bitrate in kbps.

    TURN relay shares a small VPS uplink; uncapped ~3 Mbps bursts queue there
    and show up as receiver jitter-buffer delay. P2P can spend more.
    """
    if relay:
        return (200, 400, 800) if stressed else (250, 500, 1000)
    return (300, 800, 1500) if stressed else (400, 1000, 2000)


def apply_video_bitrate_fmtp(sdp: str, min_kbps: int, start_kbps: int, max_kbps: int) -> str:
    """Annotate video ``a=fmtp`` lines with Chrome ``x-google-*-bitrate`` hints.

    RTX/FEC ``fmtp`` lines (``apt=``) are left untouched. Audio sections are
    skipped. Existing hints are not duplicated.
    """
    extra = (
        f"x-google-min-bitrate={int(min_kbps)};"
        f"x-google-start-bitrate={int(start_kbps)};"
        f"x-google-max-bitrate={int(max_kbps)}"
    )
    newline = "\r\n" if "\r\n" in sdp else "\n"
    ended_with_nl = sdp.endswith("\n")
    lines = sdp.replace("\r\n", "\n").split("\n")
    if lines and lines[-1] == "":
        lines = lines[:-1]
        ended_with_nl = True
    in_video = False
    out: list[str] = []
    for line in lines:
        if line.startswith("m="):
            in_video = line.startswith("m=video")
        if (
            in_video
            and line.startswith("a=fmtp:")
            and "x-google-max-bitrate" not in line
            and "apt=" not in line
        ):
            line = f"{line};{extra}"
        out.append(line)
    text = newline.join(out)
    if ended_with_nl:
        text += newline
    return text

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

# Scene-change I-frames are far larger than the average bitrate. Size the
# first intra so it can leave the sender in 80–120ms at the current cap,
# then step resolution back up. 0.75 bpp matches a typical VP8/H.264
# scene-change intra (average inter frames are much cheaper).
KEYFRAME_BUDGET_MS = 100
KEYFRAME_BUDGET_MIN_MS = 80
KEYFRAME_BUDGET_MAX_MS = 120
KEYFRAME_BITS_PER_PIXEL = 0.75
SCENE_DIFF_THRESHOLD = 0.16
SCENE_STEP_MS = 200
SCENE_RESTORE_MS = 350
LUMA_SAMPLE_W = 32
LUMA_SAMPLE_H = 18


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


def should_drop_until_keyframe(*, jb_ms: float | None = None, scene_change: bool = False) -> bool:
    """True when the viewer should discard delta frames and wait for the next intra."""
    if scene_change:
        return True
    return jb_ms is not None and jb_ms >= BUFFER_CATCHUP_HARD_MS


def even_dim(n: int, minimum: int = 2) -> int:
    """Even positive dimension (encoders reject odd YUV sizes)."""
    value = max(int(minimum), int(n))
    if value % 2:
        value -= 1
    return max(int(minimum) + int(minimum) % 2, value)


def keyframe_budget_bytes(bitrate_bps: int, budget_ms: float = KEYFRAME_BUDGET_MS) -> int:
    """Bytes an I-frame may occupy to finish sending in ``budget_ms``."""
    bps = max(1, int(bitrate_bps))
    ms = min(KEYFRAME_BUDGET_MAX_MS, max(KEYFRAME_BUDGET_MIN_MS, float(budget_ms)))
    return max(1024, int(bps * (ms / 1000.0) / 8.0))


def scene_scale_size(
    width: int,
    height: int,
    budget_bytes: int,
    *,
    bits_per_pixel: float = KEYFRAME_BITS_PER_PIXEL,
) -> tuple[int, int, float]:
    """Return ``(w, h, scale)`` so a scene-change I-frame fits ``budget_bytes``."""
    w = max(2, int(width))
    h = max(2, int(height))
    budget = max(1, int(budget_bytes))
    raw = (w * h * float(bits_per_pixel)) / 8.0
    if raw <= budget:
        return even_dim(w), even_dim(h), 1.0
    scale = (budget / raw) ** 0.5
    scale = max(0.35, min(1.0, scale))
    sw = min(even_dim(w), even_dim(round(w * scale)))
    sh = min(even_dim(h), even_dim(round(h * scale)))
    actual = (sw / w) if w else 1.0
    return sw, sh, actual


def scene_burst_ladder(
    width: int,
    height: int,
    budget_bytes: int,
) -> list[tuple[int, int, int]]:
    """Resolution steps after a switch: ``(w, h, hold_ms)``, last is full quality."""
    fw, fh = even_dim(width), even_dim(height)
    sw, sh, scale = scene_scale_size(fw, fh, budget_bytes)
    if scale >= 0.98:
        return [(fw, fh, 0)]
    mw = even_dim(round((sw + fw) / 2))
    mh = even_dim(round((sh + fh) / 2))
    steps: list[tuple[int, int, int]] = [(sw, sh, SCENE_STEP_MS)]
    if (mw, mh) not in {(sw, sh), (fw, fh)}:
        steps.append((mw, mh, max(0, SCENE_RESTORE_MS - SCENE_STEP_MS)))
    steps.append((fw, fh, 0))
    return steps


def scene_luma_diff(prev: list[int], curr: list[int]) -> float:
    """Mean absolute difference of 8-bit luma samples, in ``[0, 1]``."""
    if not prev or not curr or len(prev) != len(curr):
        return 1.0
    acc = 0
    for a, b in zip(prev, curr):
        acc += abs(int(a) - int(b))
    return acc / (len(prev) * 255.0)


def is_scene_change(diff: float, threshold: float = SCENE_DIFF_THRESHOLD) -> bool:
    return float(diff) >= float(threshold)


def catchup_stale_count(now: float, start: float, count: int, interval: float) -> int:
    """If the producer is more than one frame behind, jump to the latest tick."""
    if interval <= 0:
        return count
    target = start + count * interval
    if now - target <= interval:
        return count
    return max(count, int((now - start) / interval))


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

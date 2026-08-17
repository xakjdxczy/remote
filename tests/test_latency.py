from remote.latency import (
    KEYFRAME_BUDGET_MS,
    apply_video_bitrate_fmtp,
    catchup_stale_count,
    is_scene_change,
    keyframe_budget_bytes,
    playback_rate_for_buffer_ms,
    scene_burst_ladder,
    scene_luma_diff,
    scene_scale_size,
    should_drop_until_keyframe,
    should_request_keyframe,
    video_bitrate_kbps,
)


def test_playback_rate_stays_1x_when_buffer_is_healthy():
    assert playback_rate_for_buffer_ms(40) == 1.0
    assert playback_rate_for_buffer_ms(69) == 1.0


def test_playback_rate_catches_up_as_buffer_grows():
    assert playback_rate_for_buffer_ms(70) == 1.08
    assert playback_rate_for_buffer_ms(120) == 1.18
    assert playback_rate_for_buffer_ms(200) == 1.35


def test_keyframe_only_when_buffer_is_badly_behind():
    assert should_request_keyframe(179) is False
    assert should_request_keyframe(180) is True


def test_turn_bitrate_is_lower_than_p2p():
    p2p_min, p2p_start, p2p_max = video_bitrate_kbps(relay=False)
    turn_min, turn_start, turn_max = video_bitrate_kbps(relay=True)
    assert turn_max < p2p_max
    assert turn_start < p2p_start
    assert turn_min <= turn_start <= turn_max
    assert p2p_min <= p2p_start <= p2p_max
    # Uncapped ~3 Mbps on TURN was queuing at the VPS; stay at 1 Mbps.
    assert turn_max == 1000
    assert p2p_max == 2000


def test_stressed_caps_drop_further():
    _, _, p2p = video_bitrate_kbps(relay=False, stressed=True)
    _, _, turn = video_bitrate_kbps(relay=True, stressed=True)
    assert p2p < video_bitrate_kbps(relay=False)[2]
    assert turn < video_bitrate_kbps(relay=True)[2]


def test_fmtp_bitrate_only_on_video_codec_lines():
    sdp = "\r\n".join(
        [
            "v=0",
            "m=audio 9 UDP/TLS/RTP/SAVPF 111",
            "a=fmtp:111 minptime=10;useinbandfec=1",
            "m=video 9 UDP/TLS/RTP/SAVPF 96 97",
            "a=rtpmap:96 VP8/90000",
            "a=fmtp:96 max-fr=30",
            "a=rtpmap:97 rtx/90000",
            "a=fmtp:97 apt=96",
            "a=candidate:1 1 UDP 2122260223 192.168.1.2 9 typ host",
            "",
        ]
    )
    out = apply_video_bitrate_fmtp(sdp, 250, 500, 1000)
    assert "a=fmtp:111 minptime=10;useinbandfec=1\r\n" in out
    assert "x-google-max-bitrate=1000" in out
    assert "a=fmtp:96 max-fr=30;x-google-min-bitrate=250;x-google-start-bitrate=500;x-google-max-bitrate=1000" in out
    assert "a=fmtp:97 apt=96" in out
    assert "x-google-max-bitrate" not in out.split("a=fmtp:97")[1].split("\n")[0]
    assert "typ host" in out
    assert out.endswith("\r\n")


def test_keyframe_budget_is_80_to_120ms_of_the_cap():
    # 1 Mbps × 100ms / 8 = 12_500 bytes (the TURN switch I-frame budget).
    assert keyframe_budget_bytes(1_000_000) == 12_500
    assert keyframe_budget_bytes(2_000_000) == 25_000
    assert keyframe_budget_bytes(1_000_000, 80) == 10_000
    assert keyframe_budget_bytes(1_000_000, 120) == 15_000
    # Clamp outside the 80–120ms window.
    assert keyframe_budget_bytes(1_000_000, 40) == keyframe_budget_bytes(1_000_000, 80)
    assert keyframe_budget_bytes(1_000_000, 200) == keyframe_budget_bytes(1_000_000, 120)
    assert KEYFRAME_BUDGET_MS == 100


def test_scene_scale_shrinks_540p_on_turn_but_not_tiny_frames():
    budget = keyframe_budget_bytes(1_000_000)
    w, h, scale = scene_scale_size(540, 960, budget)
    assert scale < 1.0
    assert w < 540 and h < 960
    assert w % 2 == 0 and h % 2 == 0
    # A 160×90 tile already fits; do not shrink further.
    tw, th, tscale = scene_scale_size(160, 90, budget)
    assert tscale == 1.0
    assert (tw, th) == (160, 90)


def test_scene_burst_ladder_steps_back_to_full_quality():
    ladder = scene_burst_ladder(540, 960, keyframe_budget_bytes(1_000_000))
    assert ladder[-1][:2] == (540, 960)
    assert ladder[-1][2] == 0
    assert ladder[0][0] < 540
    assert all(w % 2 == 0 and h % 2 == 0 for w, h, _ in ladder)


def test_scene_luma_diff_detects_a_desktop_switch():
    dark = [10] * 32
    same = [12] * 32
    bright = [200] * 32
    assert scene_luma_diff(dark, same) < 0.05
    assert is_scene_change(scene_luma_diff(dark, same)) is False
    assert is_scene_change(scene_luma_diff(dark, bright)) is True
    assert scene_luma_diff([], bright) == 1.0


def test_drop_until_keyframe_on_switch_or_swollen_buffer():
    assert should_drop_until_keyframe(scene_change=True) is True
    assert should_drop_until_keyframe(jb_ms=119) is False
    assert should_drop_until_keyframe(jb_ms=120) is True
    assert should_drop_until_keyframe() is False


def test_catchup_skips_stale_ticks_and_keeps_on_time_ticks():
    start = 1000.0
    interval = 1 / 30
    assert catchup_stale_count(start + interval * 3, start, 3, interval) == 3
    # More than one frame late → jump to the latest tick.
    late = start + interval * 10 + 0.05
    jumped = catchup_stale_count(late, start, 3, interval)
    assert jumped >= 10


def test_fmtp_is_idempotent():
    sdp = "m=video 9 UDP/TLS/RTP/SAVPF 96\na=fmtp:96 max-fr=30\n"
    once = apply_video_bitrate_fmtp(sdp, 250, 500, 1000)
    twice = apply_video_bitrate_fmtp(once, 250, 500, 1000)
    assert once.count("x-google-max-bitrate") == 1
    assert twice == once

from remote.latency import (
    apply_video_bitrate_fmtp,
    playback_rate_for_buffer_ms,
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


def test_fmtp_is_idempotent():
    sdp = "m=video 9 UDP/TLS/RTP/SAVPF 96\na=fmtp:96 max-fr=30\n"
    once = apply_video_bitrate_fmtp(sdp, 250, 500, 1000)
    twice = apply_video_bitrate_fmtp(once, 250, 500, 1000)
    assert once.count("x-google-max-bitrate") == 1
    assert twice == once

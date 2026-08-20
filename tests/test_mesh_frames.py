from remote.agent_cli import _command
from remote.mesh_client import TYPE_DATA, TYPE_OPEN, pack_frame, parse_frames
from remote.urls import official_http, official_ws, ws_to_http


def test_mesh_frames_roundtrip():
    blob = pack_frame(TYPE_OPEN, 7) + pack_frame(TYPE_DATA, 7, b"ssh")
    frames = parse_frames(blob)
    assert frames == [(TYPE_OPEN, 7, b""), (TYPE_DATA, 7, b"ssh")]
    assert parse_frames(b"\x01short") == []


def test_ws_to_http():
    assert ws_to_http("wss://117.72.108.246/ws") == "https://117.72.108.246"
    assert ws_to_http("ws://127.0.0.1:8080/ws") == "http://127.0.0.1:8080"
    assert official_ws().endswith("/ws")
    assert "://" in official_http()


def test_agent_cli_command():
    class Args:
        extra = ["--", "echo", "hi"]
        command = ""

    assert _command(Args()) == "echo hi"
    Args.extra = []
    Args.command = "whoami"
    assert _command(Args()) == "whoami"

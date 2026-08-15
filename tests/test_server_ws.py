from fastapi.testclient import TestClient

from remote.server.api import create_app
from remote.server.registry import Registry
import remote.server.api as server_mod


def test_health_and_config(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    monkeypatch.setattr(server_mod, "demo_host", None)
    client = TestClient(create_app())
    health = client.get("/api/health")
    assert health.status_code == 200
    assert health.json()["ok"] is True
    cfg = client.get("/api/config")
    assert cfg.json()["mode"] == "server"
    assert cfg.json()["transport"] == "p2p"
    assert cfg.json()["demo_host"] is None
    for server in cfg.json()["ice_servers"]:
        for url in server["urls"]:
            assert url.startswith("stun:")


def test_register_connect_and_signal_only(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())

    with client.websocket_connect("/ws") as host, client.websocket_connect("/ws") as viewer:
        host.send_json({
            "type": "register",
            "hostname": "demo",
            "os": "Linux",
            "device_id": "123123123",
            "temp_password": "passw0rd",
        })
        registered = host.receive_json()
        assert registered["type"] == "registered"
        assert registered["device_id"] == "123123123"

        viewer.send_json({
            "type": "connect",
            "device_id": "123 123 123",
            "password": "wrong",
            "name": "alice",
        })
        failed = viewer.receive_json()
        assert failed["type"] == "auth_failed"

        viewer.send_json({
            "type": "connect",
            "device_id": "123123123",
            "password": "passw0rd",
            "name": "alice",
        })
        start_viewer = viewer.receive_json()
        start_host = host.receive_json()
        assert start_viewer["type"] == "session_start"
        assert start_host["type"] == "session_start"
        assert start_viewer["transport"] == "p2p"

        host.send_bytes(b"\x01" + b"frame")
        viewer.send_json({"type": "input", "event": "move", "x": 10, "y": 20})
        rejected = viewer.receive_json()
        assert rejected["type"] == "error"

        viewer.send_json({
            "type": "signal",
            "kind": "offer",
            "sdp": {"type": "offer", "sdp": "v=0"},
        })
        forwarded = host.receive_json()
        assert forwarded["type"] == "signal"
        assert forwarded["kind"] == "offer"

        host.send_json({
            "type": "signal",
            "kind": "answer",
            "sdp": {"type": "answer", "sdp": "v=0"},
        })
        answer = viewer.receive_json()
        assert answer["type"] == "signal"
        assert answer["kind"] == "answer"

from fastapi.testclient import TestClient

from remote.server.api import WEB_DIR, create_app, web_dir
from remote.server.oss import OssError
from remote.server.registry import Registry
import remote.server.api as server_mod


def test_web_dir_points_at_package_assets():
    assert WEB_DIR == web_dir()
    assert (WEB_DIR / "index.html").is_file()
    assert (WEB_DIR / "js" / "cam.js").is_file()


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
    assert cfg.json()["desktop_app"] is False
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
        pending = viewer.receive_json()
        incoming = host.receive_json()
        assert pending["type"] == "call_pending"
        assert incoming["type"] == "incoming_call"
        assert incoming["viewer_name"] == "alice"
        host.send_json({"type": "auth_result", "session_id": incoming["session_id"], "ok": True})
        start_host = host.receive_json()
        start_viewer = viewer.receive_json()
        assert start_viewer["type"] == "session_start"
        assert start_host["type"] == "session_start"
        assert start_viewer["transport"] == "p2p"
        assert start_viewer["host_id"] == "123123123"

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


def test_android_download_json_and_redirect(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())

    def fake_payload():
        return {
            "ok": True,
            "url": "https://bucket.example/app.apk?sig=1",
            "filename": "remotedesk-android.apk",
            "expires_in": 600,
            "size": 12,
            "version": "1.8.1",
            "version_code": 15,
            "sha256": "abc",
            "uploaded_at": "2026-08-18T00:00:00Z",
        }

    monkeypatch.setattr(server_mod.oss, "apk_download_payload", fake_payload)
    client = TestClient(create_app())
    data = client.get("/api/downloads/android")
    assert data.status_code == 200
    assert data.json()["url"].startswith("https://bucket.example/")
    assert data.json()["filename"] == "remotedesk-android.apk"
    bounced = client.get("/api/downloads/android?redirect=1", follow_redirects=False)
    assert bounced.status_code == 302
    assert bounced.headers["location"] == "https://bucket.example/app.apk?sig=1"


def test_android_download_unavailable(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    monkeypatch.setattr(
        server_mod.oss,
        "apk_download_payload",
        lambda: (_ for _ in ()).throw(OssError("OSS is not configured")),
    )
    client = TestClient(create_app())
    res = client.get("/api/downloads/android")
    assert res.status_code == 503
    assert res.json()["ok"] is False


def test_host_can_reject_incoming_call(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())
    with client.websocket_connect("/ws") as host, client.websocket_connect("/ws") as viewer:
        host.send_json({
            "type": "register",
            "hostname": "demo",
            "os": "Linux",
            "device_id": "321321321",
            "temp_password": "passw0rd",
        })
        assert host.receive_json()["type"] == "registered"
        viewer.send_json({
            "type": "connect",
            "device_id": "321321321",
            "password": "passw0rd",
            "name": "bob",
        })
        assert viewer.receive_json()["type"] == "call_pending"
        incoming = host.receive_json()
        assert incoming["type"] == "incoming_call"
        host.send_json({"type": "auth_result", "session_id": incoming["session_id"], "ok": False})
        ended = viewer.receive_json()
        assert ended["type"] == "session_end"
        assert ended["reason"] == "rejected"

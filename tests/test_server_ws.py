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


def test_config_allows_desktop_origin(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    monkeypatch.setattr(server_mod, "demo_host", None)
    client = TestClient(create_app())
    cfg = client.get("/api/config", headers={"Origin": "http://127.0.0.1:18790"})
    assert cfg.status_code == 200
    assert cfg.headers.get("access-control-allow-origin") in {"*", "http://127.0.0.1:18790"}


def test_app_js_and_iframe_header(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())
    js = client.get("/api/app.js")
    assert js.status_code == 200
    assert "DUSTX_SIGNAL_HTTP" in js.text
    page = client.get("/")
    assert page.headers.get("content-security-policy") == "frame-ancestors *"


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


def test_desktop_download_json_and_unknown_kind(monkeypatch, tmp_path):
    monkeypatch.setattr(server_mod, "registry", Registry())

    def fake_payload(kind):
        return {
            "ok": True,
            "url": f"https://bucket.example/{kind}.zip?sig=1",
            "filename": f"{kind}.zip",
            "expires_in": 600,
        }

    monkeypatch.setattr(server_mod.oss, "download_payload", fake_payload)
    monkeypatch.setenv("DUSTX_DATA_DIR", str(tmp_path))
    client = TestClient(create_app())
    mac = client.get("/api/downloads/macos")
    assert mac.status_code == 200
    assert mac.json()["filename"] == "macos.zip"
    win = client.get("/api/downloads/windows?redirect=1", follow_redirects=False)
    assert win.status_code == 302
    assert win.headers["location"].endswith("windows.zip?sig=1")
    assert client.get("/api/downloads/nope").status_code == 404


def test_download_click_counts_post_and_redirect(monkeypatch, tmp_path):
    monkeypatch.setattr(server_mod, "registry", Registry())
    monkeypatch.setenv("DUSTX_DATA_DIR", str(tmp_path))

    def fake_payload(kind):
        return {"ok": True, "url": f"https://bucket.example/{kind}.zip", "filename": f"{kind}.zip"}

    monkeypatch.setattr(server_mod.oss, "download_payload", fake_payload)
    client = TestClient(create_app())
    from remote.server import download_stats

    assert client.get("/api/downloads/macos").status_code == 200
    assert download_stats.counts()["macos"] == 0
    assert client.post("/api/downloads/macos").status_code == 200
    assert download_stats.counts()["macos"] == 1
    bounced = client.get("/api/downloads/windows?redirect=1", follow_redirects=False)
    assert bounced.status_code == 302
    assert download_stats.counts()["windows"] == 1
    assert download_stats.counts()["android"] == 0


def test_download_daily_quota_returns_busy(monkeypatch, tmp_path):
    monkeypatch.setattr(server_mod, "registry", Registry())
    monkeypatch.setenv("DUSTX_DATA_DIR", str(tmp_path))
    monkeypatch.setenv("DUSTX_DOWNLOAD_DAILY_BYTES", "100")

    def fake_payload(kind):
        return {
            "ok": True,
            "url": f"https://bucket.example/{kind}.zip",
            "filename": f"{kind}.zip",
            "size": 100,
        }

    monkeypatch.setattr(server_mod.oss, "download_payload", fake_payload)
    client = TestClient(create_app())
    from remote.server import download_stats

    first = client.post("/api/downloads/macos")
    assert first.status_code == 200
    second = client.post("/api/downloads/macos")
    assert second.status_code == 503
    body = second.json()
    assert body["busy"] is True
    assert "繁忙" in body["message"]
    assert download_stats.counts()["macos"] == 1


def test_hosted_download_quota_counts_other_software(monkeypatch, tmp_path):
    monkeypatch.setattr(server_mod, "registry", Registry())
    monkeypatch.setenv("DUSTX_DATA_DIR", str(tmp_path))
    www = tmp_path / "www"
    apk = www / "board" / "android" / "boardime.apk"
    apk.parent.mkdir(parents=True)
    apk.write_bytes(b"apk-bytes-here")
    monkeypatch.setenv("DUSTX_WWW_ROOT", str(www))
    client = TestClient(create_app())
    from remote.server import download_stats

    denied = client.get("/api/download-quota")
    assert denied.status_code == 404
    headers = {
        "X-Quota-Internal": "1",
        "X-Original-URI": "/board/android/boardime.apk",
    }
    ok = client.get("/api/download-quota", headers=headers)
    assert ok.status_code == 204
    assert download_stats.counts()["hosted"] == 1
    missing = client.get(
        "/api/download-quota",
        headers={"X-Quota-Internal": "1", "X-Original-URI": "/board/android/missing.apk"},
    )
    assert missing.status_code == 204
    assert download_stats.counts()["hosted"] == 1


def test_hosted_download_quota_shares_daily_cap(monkeypatch, tmp_path):
    monkeypatch.setattr(server_mod, "registry", Registry())
    monkeypatch.setenv("DUSTX_DATA_DIR", str(tmp_path))
    monkeypatch.setenv("DUSTX_DOWNLOAD_DAILY_BYTES", "10")
    www = tmp_path / "www"
    apk = www / "board" / "android" / "boardime.apk"
    apk.parent.mkdir(parents=True)
    apk.write_bytes(b"apk-bytes-here")
    monkeypatch.setenv("DUSTX_WWW_ROOT", str(www))

    def fake_payload(kind):
        return {
            "ok": True,
            "url": f"https://bucket.example/{kind}.zip",
            "filename": f"{kind}.zip",
            "size": 10,
        }

    monkeypatch.setattr(server_mod.oss, "download_payload", fake_payload)
    client = TestClient(create_app())
    from remote.server import download_stats

    assert client.post("/api/downloads/macos").status_code == 200
    busy = client.get(
        "/api/download-quota",
        headers={
            "X-Quota-Internal": "1",
            "X-Original-URI": "/board/android/boardime.apk",
        },
    )
    assert busy.status_code == 403
    assert busy.json()["busy"] is True
    assert download_stats.counts()["hosted"] == 0


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


def test_mesh_host_accepts_two_viewers(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())
    with client.websocket_connect("/ws") as host, client.websocket_connect("/ws") as a, client.websocket_connect("/ws") as b:
        host.send_json({
            "type": "register",
            "hostname": "pc-a",
            "os": "macOS",
            "device_id": "555666777",
            "temp_password": "passw0rd",
        })
        assert host.receive_json()["type"] == "registered"
        a.send_json({"type": "connect", "device_id": "555666777", "password": "passw0rd", "name": "尘埃X-mesh"})
        assert a.receive_json()["type"] == "call_pending"
        incoming_a = host.receive_json()
        host.send_json({"type": "auth_result", "session_id": incoming_a["session_id"], "ok": True})
        assert host.receive_json()["type"] == "session_start"
        assert a.receive_json()["type"] == "session_start"
        b.send_json({"type": "connect", "device_id": "555666777", "password": "passw0rd", "name": "尘埃X-mesh"})
        pending_b = b.receive_json()
        assert pending_b["type"] == "call_pending"
        incoming_b = host.receive_json()
        assert incoming_b["type"] == "incoming_call"
        host.send_json({"type": "auth_result", "session_id": incoming_b["session_id"], "ok": True})
        assert host.receive_json()["type"] == "session_start"
        assert b.receive_json()["type"] == "session_start"
        a.send_json({"type": "signal", "session_id": incoming_a["session_id"], "kind": "offer", "sdp": {"type": "offer", "sdp": "v=0"}})
        forwarded = host.receive_json()
        assert forwarded["session_id"] == incoming_a["session_id"]
        assert forwarded["kind"] == "offer"


def test_one_code_allows_mesh_and_remote(monkeypatch):
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())
    with client.websocket_connect("/ws") as host, client.websocket_connect("/ws") as mesh, client.websocket_connect("/ws") as remote:
        host.send_json({
            "type": "register",
            "hostname": "MacBook",
            "os": "macOS",
            "device_id": "555666777",
            "temp_password": "passw0rd",
        })
        assert host.receive_json()["type"] == "registered"
        mesh.send_json({"type": "connect", "device_id": "555666777", "password": "passw0rd", "name": "尘埃X-mesh"})
        assert mesh.receive_json()["type"] == "call_pending"
        incoming_mesh = host.receive_json()
        host.send_json({"type": "auth_result", "session_id": incoming_mesh["session_id"], "ok": True})
        assert host.receive_json()["type"] == "session_start"
        assert mesh.receive_json()["type"] == "session_start"
        remote.send_json({"type": "connect", "device_id": "555666777", "password": "passw0rd", "name": "alice"})
        assert remote.receive_json()["type"] == "call_pending"
        incoming_remote = host.receive_json()
        assert incoming_remote["type"] == "incoming_call"
        assert incoming_remote["viewer_name"] == "alice"
        host.send_json({"type": "auth_result", "session_id": incoming_remote["session_id"], "ok": True})
        assert host.receive_json()["type"] == "session_start"
        assert remote.receive_json()["type"] == "session_start"
        remote.send_json({
            "type": "signal",
            "session_id": incoming_remote["session_id"],
            "kind": "offer",
            "sdp": {"type": "offer", "sdp": "v=0"},
        })
        forwarded = host.receive_json()
        assert forwarded["session_id"] == incoming_remote["session_id"]
        assert forwarded["kind"] == "offer"

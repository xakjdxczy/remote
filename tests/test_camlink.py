from remote.server import camlink
from remote.server.api import create_app
from remote.server.camlink import CamHub
from remote.server.registry import Registry
from fastapi.testclient import TestClient
import remote.server.api as server_mod


def _desktop(monkeypatch):
    monkeypatch.setenv("DUSTX_DESKTOP", "1")


def test_cam_hidden_on_web_console(monkeypatch):
    monkeypatch.delenv("DUSTX_DESKTOP", raising=False)
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())
    assert client.get("/api/config").json()["desktop_app"] is False
    assert client.get("/api/cam").status_code == 404
    assert client.post("/api/cam/rotate").status_code == 404
    assert client.post("/api/cam/adb").status_code == 404
    assert client.post("/api/cam/sink/start").status_code == 404
    assert client.post("/api/cam/sink/stop").status_code == 404


def test_cam_info_and_rotate(monkeypatch):
    _desktop(monkeypatch)
    hub = CamHub()
    monkeypatch.setattr(camlink, "hub", hub)
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())
    info = client.get("/api/cam")
    assert info.status_code == 200
    body = info.json()
    assert body["ok"] is True
    assert len(body["token"]) == 6
    assert "adb reverse" in body["usb"]["adb_reverse"]
    assert body["usb"]["pair_url"].startswith("dustcam://127.0.0.1")
    for url in body.get("pair_urls") or []:
        assert url.startswith("dustcam://")
        assert body["token"] in url
    old = body["token"]
    rotated = client.post("/api/cam/rotate")
    assert rotated.json()["token"] != old


def test_cam_ws_token_and_signal(monkeypatch):
    _desktop(monkeypatch)
    hub = CamHub()
    monkeypatch.setattr(camlink, "hub", hub)
    monkeypatch.setattr(server_mod, "registry", Registry())
    client = TestClient(create_app())
    token = hub.token

    with client.websocket_connect("/cam/ws") as desktop, client.websocket_connect("/cam/ws") as phone:
        desktop.send_json({"type": "hello", "role": "desktop", "token": "000000"})
        assert desktop.receive_json()["type"] == "error"

        desktop.send_json({"type": "hello", "role": "desktop", "token": token})
        assert desktop.receive_json()["type"] == "hello_ok"

        phone.send_json({"type": "hello", "role": "phone", "token": token})
        assert phone.receive_json()["type"] == "hello_ok"
        assert desktop.receive_json()["type"] == "ready"
        assert phone.receive_json()["type"] == "ready"

        desktop.send_json({"type": "signal", "kind": "offer", "sdp": {"type": "offer", "sdp": "v=0"}})
        forwarded = phone.receive_json()
        assert forwarded["type"] == "signal"
        assert forwarded["kind"] == "offer"


def test_cam_sink_toggle(monkeypatch):
    _desktop(monkeypatch)
    hub = CamHub()
    monkeypatch.setattr(camlink, "hub", hub)
    monkeypatch.setattr(server_mod, "registry", Registry())

    started = {}

    def fake_start(url, token):
        started["url"] = url
        started["token"] = token
        return {"ok": True, "running": True, "message": "ok"}

    monkeypatch.setattr("remote.cam_sink.start_background", fake_start)
    monkeypatch.setattr("remote.cam_sink.stop_background", lambda: {"ok": True, "running": False})
    client = TestClient(create_app())
    assert client.post("/api/cam/sink/start").json()["ok"] is True
    assert started["token"] == hub.token
    assert client.post("/api/cam/sink/stop").json()["running"] is False


def test_cam_adb_without_binary(monkeypatch):
    _desktop(monkeypatch)
    hub = CamHub()
    monkeypatch.setattr(camlink, "hub", hub)
    monkeypatch.setattr(server_mod, "registry", Registry())

    def boom(*_a, **_k):
        raise FileNotFoundError("adb")

    monkeypatch.setattr(camlink.asyncio, "create_subprocess_exec", boom)
    client = TestClient(create_app())
    res = client.post("/api/cam/adb")
    assert res.status_code == 200
    assert res.json()["ok"] is False

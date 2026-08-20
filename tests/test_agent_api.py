import asyncio

from fastapi.testclient import TestClient

from remote.protocol import decode_json
from remote.server.api import _dispatch_agent, _on_agent_result, create_app
from remote.server.registry import Registry
import remote.server.api as server_mod


class FakeWS:
    def __init__(self, reply=True):
        self.reply = reply
        self.sent: list[dict] = []

    async def send_text(self, raw: str) -> None:
        msg = decode_json(raw)
        self.sent.append(msg)
        if self.reply and msg.get("type") == "agent":
            await _on_agent_result(
                self,
                {
                    "type": "agent_result",
                    "id": msg["id"],
                    "ok": True,
                    "op": msg.get("op"),
                    "entries": [{"name": "a", "dir": True, "size": 0}],
                    "stdout": "user\n",
                    "exit": 0,
                    "stderr": "",
                },
            )


def _install(monkeypatch) -> Registry:
    registry = Registry()
    monkeypatch.setattr(server_mod, "registry", registry)
    monkeypatch.setattr(server_mod, "_agent_pending", {})
    return registry


def test_agent_offline_and_bad_request(monkeypatch):
    _install(monkeypatch)
    client = TestClient(create_app())
    missing = client.post("/api/agent", json={"op": "list"})
    assert missing.status_code == 400
    offline = client.post(
        "/api/agent",
        json={"device_id": "123123123", "password": "passw0rd", "op": "list"},
    )
    assert offline.status_code == 404
    assert offline.json()["error"] == "device offline"


def test_agent_wrong_password(monkeypatch):
    _install(monkeypatch)
    client = TestClient(create_app())
    with client.websocket_connect("/ws") as host:
        host.send_json({
            "type": "register",
            "hostname": "demo",
            "os": "Linux",
            "device_id": "123123123",
            "temp_password": "passw0rd",
        })
        assert host.receive_json()["type"] == "registered"
        bad = client.post(
            "/api/agent",
            json={"device_id": "123123123", "password": "nope", "op": "list"},
        )
        assert bad.status_code == 403
        assert bad.json()["error"] == "wrong password"


def test_agent_roundtrip_without_p2p(monkeypatch):
    registry = _install(monkeypatch)
    ws = FakeWS()
    registry.register_host(ws, "demo", "Linux", preferred_id="123123123", temp_password="passw0rd")

    async def go():
        return await _dispatch_agent({
            "device_id": "123 123 123",
            "password": "passw0rd",
            "op": "list",
            "path": "",
        })

    data = asyncio.run(go())
    assert data["ok"] is True
    assert data["entries"][0]["name"] == "a"
    assert ws.sent[0]["type"] == "agent"
    assert ws.sent[0]["op"] == "list"


def test_agent_works_while_session_busy(monkeypatch):
    registry = _install(monkeypatch)
    host_ws = FakeWS()
    viewer_ws = object()
    host = registry.register_host(
        host_ws, "demo", "Linux", preferred_id="123123123", temp_password="passw0rd"
    )
    registry.create_session(host, viewer_ws, "尘埃X-mesh")

    async def go():
        return await _dispatch_agent({
            "device_id": "123123123",
            "password": "passw0rd",
            "op": "exec",
            "command": "whoami",
        })

    data = asyncio.run(go())
    assert data["ok"] is True
    assert data["stdout"] == "user\n"
    assert host_ws.sent[0]["op"] == "exec"
    assert host.session_id  # P2P session still held


def test_agent_timeout(monkeypatch):
    registry = _install(monkeypatch)
    monkeypatch.setattr(server_mod, "AGENT_TIMEOUT_SEC", 0.05)
    ws = FakeWS(reply=False)
    registry.register_host(ws, "demo", "Linux", preferred_id="123123123", temp_password="passw0rd")

    async def go():
        return await _dispatch_agent({
            "device_id": "123123123",
            "password": "passw0rd",
            "op": "list",
        })

    resp = asyncio.run(go())
    assert resp.status_code == 504
    assert resp.body and b"timeout" in resp.body

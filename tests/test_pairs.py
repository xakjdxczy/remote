from fastapi.testclient import TestClient

from remote.server.api import create_app
from remote.server.registry import Registry
import remote.server.api as server_mod


def _client(monkeypatch) -> tuple[TestClient, Registry]:
    registry = Registry()
    monkeypatch.setattr(server_mod, "registry", registry)
    return TestClient(create_app()), registry


def test_pair_survives_password_refresh(monkeypatch):
    client, registry = _client(monkeypatch)
    with client.websocket_connect("/ws") as host, client.websocket_connect("/ws") as viewer:
        host.send_json({
            "type": "register",
            "hostname": "pc",
            "os": "Windows",
            "device_id": "111222333",
            "temp_password": "oldpass1",
        })
        assert host.receive_json()["type"] == "registered"
        viewer.send_json({
            "type": "register",
            "hostname": "mac",
            "os": "macOS",
            "device_id": "444555666",
            "temp_password": "viewer12",
        })
        assert viewer.receive_json()["type"] == "registered"

        viewer.send_json({"type": "pair", "device_id": "111222333", "password": "oldpass1"})
        paired = viewer.receive_json()
        assert paired["type"] == "paired"
        token = paired["token"]
        assert token
        assert registry.is_paired("444555666", "111222333")

        host.send_json({"type": "refresh_password"})
        refreshed = host.receive_json()
        assert refreshed["type"] == "password"
        assert refreshed["temp_password"] != "oldpass1"

        viewer.send_json({"type": "pair", "device_id": "111222333"})
        again = viewer.receive_json()
        assert again["type"] == "paired"
        assert again["token"] == token

        viewer.send_json({
            "type": "connect",
            "device_id": "111222333",
            "password": "",
            "name": "尘埃X-mesh",
        })
        pending = viewer.receive_json()
        assert pending["type"] == "call_pending"
        incoming = host.receive_json()
        assert incoming["type"] == "incoming_call"

        viewer.send_json({"type": "unpair", "device_id": "111222333"})
        assert viewer.receive_json()["type"] == "unpaired"
        dropped = host.receive_json()
        assert dropped["type"] == "unpaired"

        viewer.send_json({"type": "pair", "device_id": "111222333", "password": "oldpass1"})
        failed = viewer.receive_json()
        assert failed["type"] == "auth_failed"
        assert failed["message"] == "wrong password"


def test_agent_accepts_pair_token(monkeypatch):
    client, registry = _client(monkeypatch)
    with client.websocket_connect("/ws") as host:
        host.send_json({
            "type": "register",
            "hostname": "pc",
            "os": "Windows",
            "device_id": "123123123",
            "temp_password": "passw0rd",
        })
        assert host.receive_json()["type"] == "registered"
        token = registry.pair("444555666", "123123123")
        bad = client.post(
            "/api/agent",
            json={"device_id": "123123123", "from_id": "444555666", "token": "nope", "op": "list"},
        )
        assert bad.status_code == 403
        assert registry.pair_ok("444555666", "123123123", token)
        assert not registry.pair_ok("444555666", "123123123", "nope")

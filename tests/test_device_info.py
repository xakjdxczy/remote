from remote.device_info import APP_VERSION, collect_info, sanitize_info
from remote.server.api import _presence_entry, create_app
from remote.server.registry import Registry
import remote.server.api as server_mod

from fastapi.testclient import TestClient


def test_sanitize_keeps_known_fields_and_clips():
    info = sanitize_info({
        "cpu": "  13th Gen Intel(R) Core(TM) i7-13650HX  ",
        "ram_bytes": 16857746636,
        "ram_used_bytes": 8 * 1024 ** 3,
        "gpu": "NVIDIA GeForce RTX 4070 Laptop GPU",
        "disks": [{"name": "C:", "path": "C:\\", "total": 1024 ** 4, "used": 400 * 1024 ** 3, "kind": "ssd"}],
        "drop": "nope",
        "cpu_cores": 20,
    })
    assert info["cpu"].startswith("13th Gen")
    assert info["gpu"].startswith("NVIDIA")
    assert info["version"] == APP_VERSION
    assert "drop" not in info
    assert info["disks"][0]["total"] == 1024 ** 4
    assert info["cpu_cores"] == 20


def test_collect_info_has_version_and_os():
    info = collect_info()
    assert info["version"] == APP_VERSION
    assert info.get("os") or info.get("hostname")


def test_presence_includes_info_and_ip(monkeypatch):
    registry = Registry()
    monkeypatch.setattr(server_mod, "registry", registry)
    client = TestClient(create_app())
    with client.websocket_connect("/ws") as host:
        host.send_json({
            "type": "register",
            "hostname": "DustX-mesh",
            "os": "Windows",
            "device_id": "123123123",
            "temp_password": "passw0rd",
            "info": {
                "hostname": "CZHYORPC",
                "cpu": "13th Gen Intel(R) Core(TM) i7-13650HX",
                "ram_bytes": 15 * 1024 ** 3,
                "os": "Microsoft Windows 11 家庭中文版",
                "gpu": "NVIDIA GeForce RTX 4070 Laptop GPU",
                "board": "8BAB",
            },
        })
        assert host.receive_json()["type"] == "registered"
        data = client.post("/api/presence", json={"ids": ["123123123"]}).json()
        row = data["devices"]["123123123"]
        assert row["online"] is True
        assert row["hostname"] == "CZHYORPC"
        assert row["info"]["cpu"].startswith("13th Gen")
        assert row["info"]["board"] == "8BAB"
        assert "ip" in row
from fastapi.testclient import TestClient

from remote.server.api import create_app
from remote.server.oss import OssError
from remote.server.update import load_policy, save_policy, update_hint, update_payload
from remote.versioning import version_less


def test_version_less():
    assert version_less("2026.8.21.3", "2026.8.21.4")
    assert not version_less("2026.8.21.4", "2026.8.21.4")
    assert not version_less("2026.8.21.4", "2026.8.21.3")


def test_force_hint_only_when_client_is_behind(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="2026.8.21.6", notes="必须更新")
    assert load_policy()["force"] is True
    behind = update_hint("2026.8.21.4")
    assert behind["force"] is True
    assert behind["policy"] is True
    assert behind["required"] == "2026.8.21.6"
    current = update_hint("2026.8.21.6")
    assert current["force"] is False
    assert current["policy"] is True
    save_policy(force=False)
    assert update_hint("2026.8.21.4")["force"] is False


def test_update_payload_marks_newer(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=False)

    def fake_payload(kind: str):
        return {
            "ok": True,
            "url": "https://example.test/pkg",
            "filename": "dustx-macos.zip",
            "sha256": "abc",
            "size": 12,
            "version": "2026.8.21.5",
        }

    monkeypatch.setattr("remote.server.oss.download_payload", fake_payload)
    data = update_payload("macos", "2026.8.21.4")
    assert data["ok"] is True
    assert data["newer"] is True
    assert data["force"] is False
    assert data["latest"] == "2026.8.21.5"


def test_force_uses_latest_when_required_blank(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="")
    monkeypatch.setattr(
        "remote.server.oss.download_payload",
        lambda kind: {"ok": True, "url": "https://example.test/pkg", "filename": "x.zip", "sha256": "a", "size": 1, "version": "2026.8.21.5"},
    )
    data = update_payload("windows", "2026.8.21.4")
    assert data["force"] is True
    assert data["newer"] is True


def test_force_follows_newer_package_when_required_is_stale(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="2026.8.21.6")
    monkeypatch.setattr(
        "remote.server.oss.download_payload",
        lambda kind: {"ok": True, "url": "https://example.test/pkg", "filename": "x.zip", "sha256": "a", "size": 1, "version": "2026.8.21.8"},
    )
    data = update_payload("windows", "2026.8.21.6")
    assert data["newer"] is True
    assert data["force"] is True
    assert data["latest"] == "2026.8.21.8"


def test_follow_published_version_raises_force_floor(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="2026.8.21.6", notes="请更新到最新版")
    from remote.server.update import follow_published_version

    follow_published_version("2026.8.21.8")
    policy = load_policy()
    assert policy["force"] is True
    assert policy["version"] == "2026.8.21.8"
    assert policy["notes"] == "请更新到最新版"


def test_force_waits_until_package_is_newer(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="2026.8.21.5")
    monkeypatch.setattr(
        "remote.server.oss.download_payload",
        lambda kind: {"ok": True, "url": "https://example.test/pkg", "filename": "x.zip", "sha256": "a", "size": 1, "version": "2026.8.21.4"},
    )
    data = update_payload("macos", "2026.8.21.4")
    assert data["newer"] is False
    assert data["force"] is False


def test_windows_old_client_omits_sha256(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="2026.8.21.9")
    monkeypatch.setattr(
        "remote.server.oss.download_payload",
        lambda kind: {
            "ok": True,
            "url": "https://example.test/pkg",
            "filename": "x.zip",
            "sha256": "abc123",
            "size": 1,
            "version": "2026.8.21.9",
        },
    )
    old = update_payload("windows", "2026.8.21.6")
    assert old["force"] is True
    assert old.get("sha256") in (None, "")
    new = update_payload("windows", "2026.8.21.9")
    assert new.get("sha256") == "abc123"
    mac = update_payload("macos", "2026.8.21.6")
    assert mac.get("sha256") == "abc123"


def test_same_version_clears_force_and_notes(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="2026.8.21.8", notes="请更新到最新版")
    monkeypatch.setattr(
        "remote.server.oss.download_payload",
        lambda kind: {
            "ok": True,
            "url": "https://example.test/pkg",
            "filename": "x.zip",
            "sha256": "a",
            "size": 1,
            "version": "2026.8.21.8",
        },
    )
    data = update_payload("windows", "2026.8.21.8")
    assert data["newer"] is False
    assert data["force"] is False
    assert data["latest"] == "2026.8.21.8"
    assert not data.get("notes")


def test_missing_package_never_forces(monkeypatch):
    monkeypatch.setenv("DUSTX_UPDATE_POLICY", "memory")
    save_policy(force=True, version="2026.8.26.2")

    def boom(_kind: str):
        raise OssError("windows package is missing")

    monkeypatch.setattr("remote.server.oss.download_payload", boom)
    data = update_payload("windows", "2026.8.26.1")
    assert data["ok"] is False
    assert data["force"] is False
    assert "url" not in data or not data.get("url")


def test_official_update_endpoint(monkeypatch):
    monkeypatch.setattr(
        "remote.server.oss.download_payload",
        lambda kind: {
            "ok": True,
            "url": "https://example.test/pkg",
            "filename": "dustx-macos.zip",
            "sha256": "abc",
            "size": 12,
            "version": "2026.8.21.5",
        },
    )
    save_policy(force=True, version="2026.8.21.5", notes="必须更新")
    with TestClient(create_app()) as client:
        bad = client.get("/api/update")
        assert bad.status_code == 400
        data = client.get("/api/update", params={"platform": "macos", "current": "2026.8.21.4"}).json()
        assert data["ok"] is True
        assert data["force"] is True
        assert data["newer"] is True
        assert data["latest"] == "2026.8.21.5"
        assert data["url"].endswith("/api/update/file?platform=macos")

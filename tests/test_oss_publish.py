import hashlib
from pathlib import Path

import pytest

from remote.server.oss import (
    DEFAULT_WINDOWS_KEY,
    DEFAULT_WINDOWS_META_KEY,
    DEFAULT_WINDOWS_STAGING_KEY,
    OssConfig,
    OssError,
    verify_published,
)
from remote.server.oss_publish import (
    _promote_staging,
    _redact,
    _windows_put,
    parse_from_ssh,
    publish_windows_from_ssh,
)

AWS_EXAMPLE = OssConfig(
    access_key="AKIAIOSFODNN7EXAMPLE",
    secret_key="wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
    endpoint="s3.amazonaws.com",
    bucket="examplebucket",
    region="us-east-1",
)


def install_oss_store(monkeypatch):
    store: dict[str, bytes] = {}

    def _request(method, key, *, body=b"", content_type=None, meta=None, extra_query=None, timeout=300, cfg=None):
        method = method.upper()
        if method == "GET":
            if key not in store:
                raise OssError(f"OSS GET {key} failed: HTTP 404")
            return 200, store[key], {}
        if method == "HEAD":
            if key not in store:
                raise OssError(f"OSS HEAD {key} failed: HTTP 404")
            return 200, b"", {"content-length": str(len(store[key]))}
        if method == "PUT":
            store[key] = body
            return 200, b"", {}
        if method == "DELETE":
            store.pop(key, None)
            return 204, b"", {}
        raise OssError(f"OSS {method} {key} failed: unsupported in fake")

    monkeypatch.setattr("remote.server.oss._request", _request)
    return store


ZIP = b"PK\x03\x04-fake-dustx-windows-zip"


def test_parse_from_ssh_host_and_filename():
    assert parse_from_ssh("dustx-windows:DustX-windows.zip") == ("dustx-windows", "DustX-windows.zip")
    assert parse_from_ssh("dustx-windows") == ("dustx-windows", "DustX-windows.zip")
    assert parse_from_ssh("dustx-windows:") == ("dustx-windows", "DustX-windows.zip")


@pytest.mark.parametrize(
    "value",
    [
        "",
        "bad host:DustX-windows.zip",
        "dustx-windows:foo/bar.zip",
        r"dustx-windows:foo\bar.zip",
        "dustx-windows:has space.zip",
        "dustx-windows:../secret.zip",
    ],
)
def test_parse_from_ssh_rejects_unsafe(value):
    with pytest.raises(OssError):
        parse_from_ssh(value)


def test_redact_strips_presigned_url():
    text = _redact("PUT https://bucket.s3.example/k?X-Amz-Signature=deadbeef&X-Amz-Expires=7200 failed")
    assert "https://" not in text
    assert "deadbeef" not in text
    assert "[redacted-url]" in text


def test_windows_put_keeps_url_out_of_ssh(monkeypatch):
    captured: dict[str, str] = {}

    def fake_scp(host, local, remote_name, timeout=60):
        captured["cfg"] = Path(local).read_text(encoding="utf-8")
        captured["remote"] = remote_name

    def fake_ssh(host, command, timeout=60):
        captured["cmd"] = command
        return "200\n"

    monkeypatch.setattr("remote.server.oss_publish._scp_to", fake_scp)
    monkeypatch.setattr("remote.server.oss_publish._ssh", fake_ssh)
    url = "https://example-bucket.oss.example/downloads/dustx-windows.staging.bin?X-Amz-Signature=secret"
    _windows_put("dustx-windows", r"C:\Users\u\DustX-windows.zip", url)
    assert "url =" in captured["cfg"]
    assert "X-Amz-Signature=secret" in captured["cfg"]
    assert "upload-file" in captured["cfg"]
    assert "https://" not in captured["cmd"]
    assert "X-Amz" not in captured["cmd"]
    assert "secret" not in captured["cmd"]
    assert "curl.exe -K" in captured["cmd"]
    assert captured["remote"] == "dustx-oss-put.cfg"


def test_windows_put_rejects_non_200(monkeypatch):
    monkeypatch.setattr("remote.server.oss_publish._scp_to", lambda *a, **k: None)
    monkeypatch.setattr("remote.server.oss_publish._ssh", lambda *a, **k: "403")
    with pytest.raises(OssError, match="HTTP 403"):
        _windows_put("dustx-windows", r"C:\Users\u\DustX-windows.zip", "https://example/put")


def test_promote_rejects_bad_staging_without_touching_prod(monkeypatch):
    store = install_oss_store(monkeypatch)
    digest = hashlib.sha256(ZIP).hexdigest()
    store[DEFAULT_WINDOWS_STAGING_KEY] = b"not-the-zip"
    with pytest.raises(OssError, match="staging"):
        _promote_staging(digest, len(ZIP), "2026.8.21", "", AWS_EXAMPLE)
    assert DEFAULT_WINDOWS_KEY not in store
    assert DEFAULT_WINDOWS_META_KEY not in store


def test_publish_windows_put_then_get_verify(monkeypatch):
    store = install_oss_store(monkeypatch)
    digest = hashlib.sha256(ZIP).hexdigest()
    monkeypatch.setattr("remote.server.oss_publish.load_config", lambda: AWS_EXAMPLE)
    monkeypatch.setattr(
        "remote.server.oss_publish.remote_file_digest",
        lambda host, filename: (digest, len(ZIP), r"C:\Users\u\DustX-windows.zip"),
    )

    def fake_put(host, win_path, put_url, timeout=600):
        assert "put_url" not in win_path
        assert "X-Amz" in put_url
        store[DEFAULT_WINDOWS_STAGING_KEY] = ZIP

    monkeypatch.setattr("remote.server.oss_publish._windows_put", fake_put)
    info = publish_windows_from_ssh(
        "dustx-windows:DustX-windows.zip",
        version="2026.8.21",
    )
    assert info["via"] == "windows-put"
    assert info["verified"] is True
    assert info["sha256"] == digest
    assert info["size"] == len(ZIP)
    assert "put_url" not in info
    assert DEFAULT_WINDOWS_STAGING_KEY not in store
    live = verify_published("windows", digest, len(ZIP), version="2026.8.21", cfg=AWS_EXAMPLE)
    assert live["sha256"] == digest


def test_publish_falls_back_to_mac_copy_and_still_verifies(monkeypatch, tmp_path):
    store = install_oss_store(monkeypatch)
    digest = hashlib.sha256(ZIP).hexdigest()
    monkeypatch.setattr("remote.server.oss_publish.load_config", lambda: AWS_EXAMPLE)
    monkeypatch.setattr(
        "remote.server.oss_publish.remote_file_digest",
        lambda host, filename: (digest, len(ZIP), r"C:\Users\u\DustX-windows.zip"),
    )

    def fail_put(*_a, **_k):
        raise OssError("Windows PUT returned HTTP 403")

    def fake_scp(host, remote_name, local, timeout=180):
        Path(local).write_bytes(ZIP)

    monkeypatch.setattr("remote.server.oss_publish._windows_put", fail_put)
    monkeypatch.setattr("remote.server.oss_publish._scp_from", fake_scp)
    info = publish_windows_from_ssh("dustx-windows:DustX-windows.zip", version="2026.8.21")
    assert info["via"] == "mac-fallback"
    assert info["verified"] is True
    assert "403" in info["fallback_error"]
    assert info["sha256"] == digest
    verify_published("windows", digest, len(ZIP), version="2026.8.21", cfg=AWS_EXAMPLE)


def test_publish_fallback_rejects_copied_bytes_that_do_not_match(monkeypatch):
    install_oss_store(monkeypatch)
    digest = hashlib.sha256(ZIP).hexdigest()
    monkeypatch.setattr("remote.server.oss_publish.load_config", lambda: AWS_EXAMPLE)
    monkeypatch.setattr(
        "remote.server.oss_publish.remote_file_digest",
        lambda host, filename: (digest, len(ZIP), r"C:\Users\u\DustX-windows.zip"),
    )
    monkeypatch.setattr(
        "remote.server.oss_publish._windows_put",
        lambda *_a, **_k: (_ for _ in ()).throw(OssError("Windows PUT returned HTTP empty")),
    )
    monkeypatch.setattr(
        "remote.server.oss_publish._scp_from",
        lambda host, remote_name, local, timeout=180: Path(local).write_bytes(b"different"),
    )
    with pytest.raises(OssError, match="copied zip"):
        publish_windows_from_ssh("dustx-windows:DustX-windows.zip")

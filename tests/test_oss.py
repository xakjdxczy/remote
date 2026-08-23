import hashlib
import json
from datetime import datetime, timezone

from remote.server.oss import (
    DEFAULT_WINDOWS_KEY,
    DEFAULT_WINDOWS_META_KEY,
    MACOS_FILENAME,
    WINDOWS_FILENAME,
    OssConfig,
    versioned_download_name,
    OssError,
    _kind_spec,
    _xml_text,
    load_config,
    presign_get,
    presign_put,
    verify_published,
)


def install_oss_store(monkeypatch):
    """In-memory OSS. Tests must not talk to a real bucket."""
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


AWS_EXAMPLE = OssConfig(
    access_key="AKIAIOSFODNN7EXAMPLE",
    secret_key="wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
    endpoint="s3.amazonaws.com",
    bucket="examplebucket",
    region="us-east-1",
)


def test_load_config_requires_all_fields(monkeypatch, tmp_path):
    monkeypatch.delenv("OSS_AK", raising=False)
    monkeypatch.delenv("OSS_SK", raising=False)
    monkeypatch.delenv("OSS_ENDPOINT", raising=False)
    monkeypatch.delenv("OSS_BUCKET", raising=False)
    monkeypatch.setenv("DUSTX_OSS_CONFIG", str(tmp_path / "missing.ini"))
    assert load_config() is None
    ini = tmp_path / "config.ini"
    ini.write_text(
        "OSS_AK=JDC_TEST\nOSS_SK=secret\nOSS_ENDPOINT=https://s3.example-region.example-oss.com\n"
        "OSS_BUCKET=demo-bucket\nSSH_PRIVATE_KEY=ignore\n",
        encoding="utf-8",
    )
    monkeypatch.setenv("DUSTX_OSS_CONFIG", str(ini))
    cfg = load_config()
    assert cfg is not None
    assert cfg.endpoint == "s3.example-region.example-oss.com"
    assert cfg.region == "example-region"
    assert cfg.host == "demo-bucket.s3.example-region.example-oss.com"


def test_presign_is_stable_and_uses_sigv4_query():
    url = presign_get(
        "test.txt",
        expires=86400,
        now=datetime(2013, 5, 24, tzinfo=timezone.utc),
        cfg=AWS_EXAMPLE,
    )
    assert url.startswith("https://examplebucket.s3.amazonaws.com/test.txt?")
    assert "X-Amz-Algorithm=AWS4-HMAC-SHA256" in url
    assert "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request" in url
    assert "X-Amz-Expires=86400" in url
    assert "X-Amz-SignedHeaders=host" in url
    # Golden value for this signer; a real S3-compatible PUT/GET is used in deploy.
    assert "X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404" in url


def test_xml_text_extracts_upload_id():
    blob = b'<?xml version="1.0"?><InitiateMultipartUploadResult><UploadId>abc+123=</UploadId></InitiateMultipartUploadResult>'
    assert _xml_text(blob, "UploadId") == "abc+123="
    try:
        _xml_text(b"<ok/>", "UploadId")
    except OssError as exc:
        assert "UploadId" in str(exc)
    else:
        raise AssertionError("expected OssError")


def test_presign_put_is_sigv4_query():
    url = presign_put(
        "downloads/remotedesk-android.apk",
        expires=7200,
        now=datetime(2013, 5, 24, tzinfo=timezone.utc),
        cfg=AWS_EXAMPLE,
    )
    assert url.startswith("https://examplebucket.s3.amazonaws.com/downloads/remotedesk-android.apk?")
    assert "X-Amz-Algorithm=AWS4-HMAC-SHA256" in url
    assert "X-Amz-Expires=7200" in url
    assert "X-Amz-Signature=" in url


def test_versioned_download_name():
    assert versioned_download_name("windows", "2026.8.21.7", WINDOWS_FILENAME) == "DustX-windows-2026.8.21.7.zip"
    assert versioned_download_name("macos", "2026.8.21.6", MACOS_FILENAME) == "dustx-macos-2026.8.21.6.zip"
    assert versioned_download_name("android", "1.9.6", "remotedesk-android.apk") == "remotedesk-android-1.9.6.apk"
    assert versioned_download_name("windows", "", WINDOWS_FILENAME) == WINDOWS_FILENAME


def test_kind_spec_desktop_keys():
    spec = _kind_spec("macos", AWS_EXAMPLE)
    assert spec["filename"] == MACOS_FILENAME
    assert spec["key"].endswith("macos.bin")
    win = _kind_spec("windows", AWS_EXAMPLE)
    assert win["filename"] == WINDOWS_FILENAME
    try:
        _kind_spec("nope", AWS_EXAMPLE)
    except OssError as exc:
        assert "unknown" in str(exc)
    else:
        raise AssertionError("expected OssError")


def test_presign_includes_attachment_filename():
    url = presign_get(
        "downloads/remotedesk-android.apk",
        expires=600,
        filename="remotedesk-android.apk",
        now=datetime(2013, 5, 24, tzinfo=timezone.utc),
        cfg=AWS_EXAMPLE,
    )
    assert "response-content-disposition=attachment%3B%20filename%3Dremotedesk-android.apk" in url
    assert "X-Amz-Signature=" in url


def _windows_manifest(data: bytes, version: str = "2026.8.21") -> bytes:
    return json.dumps(
        {
            "key": DEFAULT_WINDOWS_KEY,
            "filename": WINDOWS_FILENAME,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "uploaded_at": "2026-08-21T00:00:00Z",
            "version": version,
            "version_code": None,
        }
    ).encode("utf-8")


def test_verify_published_requires_live_get_match(monkeypatch):
    store = install_oss_store(monkeypatch)
    data = b"dustx-windows-zip"
    digest = hashlib.sha256(data).hexdigest()
    store[DEFAULT_WINDOWS_KEY] = data
    store[DEFAULT_WINDOWS_META_KEY] = _windows_manifest(data)
    meta = verify_published("windows", digest, len(data), version="2026.8.21", cfg=AWS_EXAMPLE)
    assert meta["sha256"] == digest
    assert int(meta["size"]) == len(data)
    assert meta["version"] == "2026.8.21"


def test_verify_published_rejects_object_hash_mismatch(monkeypatch):
    store = install_oss_store(monkeypatch)
    data = b"expected-bytes"
    other = b"tampered-bytes"
    store[DEFAULT_WINDOWS_KEY] = other
    store[DEFAULT_WINDOWS_META_KEY] = _windows_manifest(data)
    try:
        verify_published("windows", hashlib.sha256(data).hexdigest(), len(data), cfg=AWS_EXAMPLE)
    except OssError as exc:
        assert "sha256 mismatch" in str(exc)
    else:
        raise AssertionError("expected OssError")


def test_verify_published_rejects_manifest_mismatch(monkeypatch):
    store = install_oss_store(monkeypatch)
    data = b"expected-bytes"
    store[DEFAULT_WINDOWS_KEY] = data
    store[DEFAULT_WINDOWS_META_KEY] = _windows_manifest(b"other-bytes")
    try:
        verify_published("windows", hashlib.sha256(data).hexdigest(), len(data), cfg=AWS_EXAMPLE)
    except OssError as exc:
        assert "manifest" in str(exc)
    else:
        raise AssertionError("expected OssError")

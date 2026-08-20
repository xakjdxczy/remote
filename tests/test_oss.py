from datetime import datetime, timezone

from remote.server.oss import (
    MACOS_FILENAME,
    WINDOWS_FILENAME,
    OssConfig,
    OssError,
    _kind_spec,
    _xml_text,
    load_config,
    presign_get,
    presign_put,
)


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
    assert "response-content-disposition=attachment%3B%20filename%3D%22remotedesk-android.apk%22" in url
    assert "X-Amz-Signature=" in url

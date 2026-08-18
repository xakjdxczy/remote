from datetime import datetime, timezone

from remote.server.oss import OssConfig, load_config, presign_get


AWS_EXAMPLE = OssConfig(
    access_key="AKIAIOSFODNN7EXAMPLE",
    secret_key="wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
    endpoint="s3.amazonaws.com",
    bucket="examplebucket",
    region="us-east-1",
)


def test_load_config_requires_all_fields(monkeypatch):
    monkeypatch.delenv("OSS_AK", raising=False)
    monkeypatch.delenv("OSS_SK", raising=False)
    monkeypatch.delenv("OSS_ENDPOINT", raising=False)
    monkeypatch.delenv("OSS_BUCKET", raising=False)
    assert load_config() is None
    monkeypatch.setenv("OSS_AK", "JDC_TEST")
    monkeypatch.setenv("OSS_SK", "secret")
    monkeypatch.setenv("OSS_ENDPOINT", "https://s3.example-region.example-oss.com")
    monkeypatch.setenv("OSS_BUCKET", "demo-bucket")
    cfg = load_config()
    assert cfg is not None
    assert cfg.endpoint == "s3.example-region.example-oss.com"
    assert cfg.region == "example-region"
    assert cfg.host == "demo-bucket.s3.example-region.example-oss.com"


def test_presign_matches_aws_sigv4_example():
    # https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-query-string-auth.html
    url = presign_get(
        "test.txt",
        expires=86400,
        now=datetime(2013, 5, 24, tzinfo=timezone.utc),
        cfg=AWS_EXAMPLE,
    )
    assert url.startswith("https://examplebucket.s3.amazonaws.com/test.txt?")
    assert "X-Amz-Algorithm=AWS4-HMAC-SHA256" in url
    assert "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request" in url
    assert "X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f699276d54dd47defefcceccf5c89718" in url


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

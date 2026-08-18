"""JD Cloud / S3-compatible OSS helper (presign + upload).

Credentials come from the environment and never appear in API responses:

* ``OSS_AK`` / ``OSS_SK`` — access key pair
* ``OSS_ENDPOINT`` — host only or URL, e.g. ``s3.example-region.example-oss.com``
* ``OSS_BUCKET`` — bucket name
* ``OSS_REGION`` — optional; inferred from the endpoint when omitted
* ``OSS_APK_KEY`` — object key for the Android package (default
  ``downloads/remotedesk-android.apk``)
"""

from __future__ import annotations

import hashlib
import hmac
import json
import mimetypes
import os
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DEFAULT_APK_KEY = "downloads/remotedesk-android.apk"
DEFAULT_META_KEY = "downloads/remotedesk-android.json"
DEFAULT_EXPIRES = 600
DOWNLOAD_FILENAME = "remotedesk-android.apk"


class OssError(RuntimeError):
    """Raised when OSS is misconfigured or a request fails."""


@dataclass(frozen=True)
class OssConfig:
    access_key: str
    secret_key: str
    endpoint: str
    bucket: str
    region: str
    apk_key: str = DEFAULT_APK_KEY
    meta_key: str = DEFAULT_META_KEY

    @property
    def host(self) -> str:
        return f"{self.bucket}.{self.endpoint}"

    @property
    def base_url(self) -> str:
        return f"https://{self.host}"


def _strip_endpoint(raw: str) -> str:
    value = raw.strip()
    for prefix in ("https://", "http://"):
        if value.lower().startswith(prefix):
            value = value[len(prefix) :]
    return value.split("/")[0].strip()


def _infer_region(endpoint: str) -> str:
    # s3.<region>.example-oss.com → <region>
    parts = endpoint.split(".")
    if len(parts) >= 2 and parts[0] == "s3":
        return parts[1]
    return "us-east-1"


def load_config() -> OssConfig | None:
    ak = (os.environ.get("OSS_AK") or "").strip()
    sk = (os.environ.get("OSS_SK") or "").strip()
    endpoint = _strip_endpoint(os.environ.get("OSS_ENDPOINT") or "")
    bucket = (os.environ.get("OSS_BUCKET") or "").strip()
    if not (ak and sk and endpoint and bucket):
        return None
    region = (os.environ.get("OSS_REGION") or "").strip() or _infer_region(endpoint)
    apk_key = (os.environ.get("OSS_APK_KEY") or DEFAULT_APK_KEY).lstrip("/")
    meta_key = (os.environ.get("OSS_META_KEY") or DEFAULT_META_KEY).lstrip("/")
    return OssConfig(
        access_key=ak,
        secret_key=sk,
        endpoint=endpoint,
        bucket=bucket,
        region=region,
        apk_key=apk_key,
        meta_key=meta_key,
    )


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _hmac(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def _signing_key(secret: str, datestamp: str, region: str, service: str) -> bytes:
    k_date = _hmac(("AWS4" + secret).encode("utf-8"), datestamp)
    k_region = _hmac(k_date, region)
    k_service = _hmac(k_region, service)
    return _hmac(k_service, "aws4_request")


def _canonical_query(params: dict[str, str]) -> str:
    items = sorted((urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(v, safe="-_.~")) for k, v in params.items())
    return "&".join(f"{k}={v}" for k, v in items)


def _canonical_uri(key: str) -> str:
    return "/" + "/".join(urllib.parse.quote(part, safe="-_.~") for part in key.split("/") if part)


def _sign_v4(
    *,
    method: str,
    cfg: OssConfig,
    key: str,
    now: datetime,
    extra_query: dict[str, str] | None = None,
    headers: dict[str, str] | None = None,
    payload_hash: str,
    expires: int | None = None,
    presign: bool = False,
) -> tuple[str, dict[str, str], dict[str, str]]:
    amz_date = now.strftime("%Y%m%dT%H%M%SZ")
    datestamp = now.strftime("%Y%m%d")
    scope = f"{datestamp}/{cfg.region}/s3/aws4_request"
    uri = _canonical_uri(key)
    signed_headers_map = {"host": cfg.host}
    if headers:
        for name, value in headers.items():
            signed_headers_map[name.lower()] = value.strip()
    if not presign:
        signed_headers_map["x-amz-content-sha256"] = payload_hash
        signed_headers_map["x-amz-date"] = amz_date

    query: dict[str, str] = dict(extra_query or {})
    if presign:
        if expires is None:
            raise OssError("presign requires expires")
        query.update(
            {
                "X-Amz-Algorithm": "AWS4-HMAC-SHA256",
                "X-Amz-Credential": f"{cfg.access_key}/{scope}",
                "X-Amz-Date": amz_date,
                "X-Amz-Expires": str(expires),
                "X-Amz-SignedHeaders": "host",
            }
        )
        signed_headers_map = {"host": cfg.host}

    signed_header_names = ";".join(sorted(signed_headers_map))
    canonical_headers = "".join(f"{k}:{signed_headers_map[k]}\n" for k in sorted(signed_headers_map))
    canonical_query = _canonical_query(query)
    canonical_request = "\n".join(
        [method, uri, canonical_query, canonical_headers, signed_header_names, payload_hash]
    )
    string_to_sign = "\n".join(
        [
            "AWS4-HMAC-SHA256",
            amz_date,
            scope,
            hashlib.sha256(canonical_request.encode("utf-8")).hexdigest(),
        ]
    )
    signature = hmac.new(
        _signing_key(cfg.secret_key, datestamp, cfg.region, "s3"),
        string_to_sign.encode("utf-8"),
        hashlib.sha256,
    ).hexdigest()

    if presign:
        query["X-Amz-Signature"] = signature
        return uri, query, {"host": cfg.host}

    auth = (
        f"AWS4-HMAC-SHA256 Credential={cfg.access_key}/{scope}, "
        f"SignedHeaders={signed_header_names}, Signature={signature}"
    )
    out_headers = {k: v for k, v in signed_headers_map.items() if k != "host"}
    out_headers["Authorization"] = auth
    if headers:
        for name, value in headers.items():
            if name.lower() not in out_headers:
                out_headers[name] = value
    return uri, query, out_headers


def presign_get(
    key: str,
    *,
    expires: int = DEFAULT_EXPIRES,
    filename: str | None = None,
    now: datetime | None = None,
    cfg: OssConfig | None = None,
) -> str:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    extra: dict[str, str] = {}
    if filename:
        extra["response-content-disposition"] = f'attachment; filename="{filename}"'
        extra["response-content-type"] = "application/vnd.android.package-archive"
    uri, query, _headers = _sign_v4(
        method="GET",
        cfg=config,
        key=key,
        now=now or _utc_now(),
        extra_query=extra,
        payload_hash="UNSIGNED-PAYLOAD",
        expires=expires,
        presign=True,
    )
    return f"{config.base_url}{uri}?{_canonical_query(query)}"


def _request(
    method: str,
    key: str,
    *,
    body: bytes = b"",
    content_type: str | None = None,
    meta: dict[str, str] | None = None,
    cfg: OssConfig,
) -> tuple[int, bytes, dict[str, str]]:
    payload_hash = hashlib.sha256(body).hexdigest()
    headers: dict[str, str] = {}
    if content_type:
        headers["content-type"] = content_type
    if meta:
        for name, value in meta.items():
            headers[f"x-amz-meta-{name}"] = value
    uri, _query, signed_headers = _sign_v4(
        method=method,
        cfg=cfg,
        key=key,
        now=_utc_now(),
        headers=headers,
        payload_hash=payload_hash,
        presign=False,
    )
    req = urllib.request.Request(
        f"{cfg.base_url}{uri}",
        data=body if method in {"PUT", "POST"} else None,
        method=method,
        headers=signed_headers,
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            raw = resp.read()
            return resp.status, raw, {k.lower(): v for k, v in resp.headers.items()}
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")[:400]
        raise OssError(f"OSS {method} {key} failed: HTTP {exc.code} {detail}") from exc


def head_object(key: str, *, cfg: OssConfig | None = None) -> dict[str, str] | None:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    try:
        _status, _body, headers = _request("HEAD", key, cfg=config)
    except OssError as exc:
        if "HTTP 404" in str(exc):
            return None
        raise
    return headers


def get_object_text(key: str, *, cfg: OssConfig | None = None) -> str | None:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    try:
        _status, body, _headers = _request("GET", key, cfg=config)
    except OssError as exc:
        if "HTTP 404" in str(exc):
            return None
        raise
    return body.decode("utf-8")


def put_object(
    key: str,
    data: bytes,
    *,
    content_type: str,
    meta: dict[str, str] | None = None,
    cfg: OssConfig | None = None,
) -> None:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    _request("PUT", key, body=data, content_type=content_type, meta=meta, cfg=config)


def upload_apk(
    path: str | Path,
    *,
    version: str = "",
    version_code: str = "",
    cfg: OssConfig | None = None,
) -> dict[str, Any]:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    apk_path = Path(path)
    if not apk_path.is_file():
        raise OssError(f"APK not found: {apk_path}")
    data = apk_path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    uploaded_at = _utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    meta = {
        "sha256": digest,
        "filename": DOWNLOAD_FILENAME,
        "uploaded-at": uploaded_at,
    }
    if version:
        meta["version"] = version
    if version_code:
        meta["version-code"] = str(version_code)
    put_object(
        config.apk_key,
        data,
        content_type="application/vnd.android.package-archive",
        meta=meta,
        cfg=config,
    )
    info = {
        "key": config.apk_key,
        "filename": DOWNLOAD_FILENAME,
        "size": len(data),
        "sha256": digest,
        "uploaded_at": uploaded_at,
        "version": version or None,
        "version_code": int(version_code) if str(version_code).isdigit() else None,
    }
    put_object(
        config.meta_key,
        json.dumps(info, ensure_ascii=False, indent=2).encode("utf-8"),
        content_type="application/json; charset=utf-8",
        cfg=config,
    )
    return info


def apk_download_payload(*, expires: int = DEFAULT_EXPIRES, cfg: OssConfig | None = None) -> dict[str, Any]:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    headers = head_object(config.apk_key, cfg=config)
    if headers is None:
        raise OssError("android package is not uploaded")
    meta: dict[str, Any] = {}
    raw_meta = get_object_text(config.meta_key, cfg=config)
    if raw_meta:
        try:
            parsed = json.loads(raw_meta)
            if isinstance(parsed, dict):
                meta = parsed
        except json.JSONDecodeError:
            meta = {}
    filename = str(meta.get("filename") or DOWNLOAD_FILENAME)
    url = presign_get(config.apk_key, expires=expires, filename=filename, cfg=config)
    size = meta.get("size")
    if size is None:
        try:
            size = int(headers.get("content-length") or 0) or None
        except ValueError:
            size = None
    return {
        "ok": True,
        "url": url,
        "filename": filename,
        "expires_in": expires,
        "size": size,
        "version": meta.get("version"),
        "version_code": meta.get("version_code"),
        "sha256": meta.get("sha256"),
        "uploaded_at": meta.get("uploaded_at"),
    }


def guess_content_type(path: Path) -> str:
    guessed, _ = mimetypes.guess_type(path.name)
    return guessed or "application/octet-stream"

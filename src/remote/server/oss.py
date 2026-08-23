"""JD Cloud / S3-compatible OSS helper (presign + upload).

Credentials come from the environment and never appear in API responses:

* ``OSS_AK`` / ``OSS_SK`` — access key pair
* ``OSS_ENDPOINT`` — host only or URL, e.g. ``s3.example-region.example-oss.com``
* ``OSS_BUCKET`` — bucket name
* ``OSS_REGION`` — optional; inferred from the endpoint when omitted
* ``OSS_APK_KEY`` — object key for the Android package (default
  ``downloads/remotedesk-android.bin``; download filename stays ``.apk``.
  JD Cloud blocks ``.apk`` on the default OSS domain.)
"""

from __future__ import annotations

import hashlib
import hmac
import json
import mimetypes
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


DEFAULT_APK_KEY = "downloads/remotedesk-android.bin"
DEFAULT_META_KEY = "downloads/remotedesk-android.json"
DEFAULT_MACOS_KEY = "downloads/dustx-macos.bin"
DEFAULT_MACOS_META_KEY = "downloads/dustx-macos.json"
DEFAULT_WINDOWS_KEY = "downloads/dustx-windows.bin"
DEFAULT_WINDOWS_META_KEY = "downloads/dustx-windows.json"
DEFAULT_WINDOWS_STAGING_KEY = "downloads/dustx-windows.staging.bin"
DEFAULT_EXPIRES = 600
DOWNLOAD_FILENAME = "remotedesk-android.apk"
MACOS_FILENAME = "dustx-macos.zip"
WINDOWS_FILENAME = "DustX-windows.zip"

DOWNLOAD_KINDS = ("android", "macos", "windows")


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


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _oss_ini_path() -> Path | None:
    override = (os.environ.get("DUSTX_OSS_CONFIG") or "").strip()
    if override:
        path = Path(override)
        return path if path.is_file() else None
    for cand in (_repo_root() / ".env" / "config.ini", Path.cwd() / ".env" / "config.ini"):
        if cand.is_file():
            return cand
    return None


def _ini_oss_values() -> dict[str, str]:
    path = _oss_ini_path()
    if path is None:
        return {}
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("OSS_") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def _oss_get(name: str, file_env: dict[str, str] | None = None) -> str:
    file_env = file_env if file_env is not None else _ini_oss_values()
    return (os.environ.get(name) or file_env.get(name) or "").strip()


def load_config() -> OssConfig | None:
    file_env = _ini_oss_values()
    ak = _oss_get("OSS_AK", file_env)
    sk = _oss_get("OSS_SK", file_env)
    endpoint = _strip_endpoint(_oss_get("OSS_ENDPOINT", file_env))
    bucket = _oss_get("OSS_BUCKET", file_env)
    if not (ak and sk and endpoint and bucket):
        return None
    region = _oss_get("OSS_REGION", file_env) or _infer_region(endpoint)
    apk_key = (_oss_get("OSS_APK_KEY", file_env) or DEFAULT_APK_KEY).lstrip("/")
    meta_key = (_oss_get("OSS_META_KEY", file_env) or DEFAULT_META_KEY).lstrip("/")
    return OssConfig(
        access_key=ak,
        secret_key=sk,
        endpoint=endpoint,
        bucket=bucket,
        region=region,
        apk_key=apk_key,
        meta_key=meta_key,
    )


def versioned_download_name(kind: str, version: str, fallback: str) -> str:
    """Put the package version in the saved filename so browsers do not reuse an old zip."""
    ver = "".join(ch for ch in str(version or "") if ch.isalnum() or ch in "._-")
    if not ver:
        return fallback
    if kind == "windows":
        return f"DustX-windows-{ver}.zip"
    if kind == "macos":
        return f"dustx-macos-{ver}.zip"
    if kind == "android":
        return f"remotedesk-android-{ver}.apk"
    return fallback


def _kind_spec(kind: str, cfg: OssConfig) -> dict[str, str]:
    name = (kind or "").strip().lower()
    if name == "android":
        return {
            "kind": "android",
            "key": cfg.apk_key,
            "meta_key": cfg.meta_key,
            "filename": DOWNLOAD_FILENAME,
            "content_type": "application/vnd.android.package-archive",
            "missing": "android package is not uploaded",
        }
    if name == "macos":
        return {
            "kind": "macos",
            "key": (os.environ.get("OSS_MACOS_KEY") or DEFAULT_MACOS_KEY).lstrip("/"),
            "meta_key": (os.environ.get("OSS_MACOS_META_KEY") or DEFAULT_MACOS_META_KEY).lstrip("/"),
            "filename": MACOS_FILENAME,
            "content_type": "application/zip",
            "missing": "macOS package is not uploaded",
        }
    if name == "windows":
        return {
            "kind": "windows",
            "key": (os.environ.get("OSS_WINDOWS_KEY") or DEFAULT_WINDOWS_KEY).lstrip("/"),
            "meta_key": (os.environ.get("OSS_WINDOWS_META_KEY") or DEFAULT_WINDOWS_META_KEY).lstrip("/"),
            "filename": WINDOWS_FILENAME,
            "content_type": "application/zip",
            "missing": "Windows package is not uploaded",
        }
    raise OssError(f"unknown download kind: {kind}")


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
    content_type: str | None = None,
    now: datetime | None = None,
    cfg: OssConfig | None = None,
) -> str:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    extra: dict[str, str] = {}
    if filename:
        extra["response-content-disposition"] = f"attachment; filename={filename}"
        extra["response-content-type"] = content_type or "application/octet-stream"
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


def presign_put(
    key: str,
    *,
    expires: int = 7200,
    now: datetime | None = None,
    cfg: OssConfig | None = None,
) -> str:
    """Time-limited PUT URL so a machine that can reach OSS can upload the APK."""
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    uri, query, _headers = _sign_v4(
        method="PUT",
        cfg=config,
        key=key,
        now=now or _utc_now(),
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
    extra_query: dict[str, str] | None = None,
    timeout: int = 300,
    cfg: OssConfig,
) -> tuple[int, bytes, dict[str, str]]:
    payload_hash = hashlib.sha256(body).hexdigest()
    headers: dict[str, str] = {}
    if content_type:
        headers["content-type"] = content_type
    if meta:
        for name, value in meta.items():
            headers[f"x-amz-meta-{name}"] = value
    uri, query, signed_headers = _sign_v4(
        method=method,
        cfg=cfg,
        key=key,
        now=_utc_now(),
        extra_query=extra_query,
        headers=headers,
        payload_hash=payload_hash,
        presign=False,
    )
    url = f"{cfg.base_url}{uri}"
    if query:
        url = f"{url}?{_canonical_query(query)}"
    req = urllib.request.Request(
        url,
        data=body if method in {"PUT", "POST"} else None,
        method=method,
        headers=signed_headers,
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
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


def get_object_bytes(key: str, *, cfg: OssConfig | None = None) -> bytes:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    _status, body, _headers = _request("GET", key, timeout=300, cfg=config)
    return body


def package_file(kind: str, *, cfg: OssConfig | None = None) -> tuple[bytes, str, str]:
    """Return (bytes, download filename, content-type) for an official package."""
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    spec = _kind_spec(kind, config)
    body = get_object_bytes(spec["key"], cfg=config)
    meta: dict[str, Any] = {}
    raw_meta = get_object_text(spec["meta_key"], cfg=config)
    if raw_meta:
        try:
            parsed = json.loads(raw_meta)
            if isinstance(parsed, dict):
                meta = parsed
        except json.JSONDecodeError:
            meta = {}
    filename = versioned_download_name(
        spec["kind"],
        str(meta.get("version") or ""),
        str(meta.get("filename") or spec["filename"]),
    )
    return body, filename, spec["content_type"]


def object_digest(key: str, *, cfg: OssConfig | None = None) -> tuple[str, int]:
    """GET the object and return (sha256 hex, size). This is the source of truth."""
    body = get_object_bytes(key, cfg=cfg)
    return hashlib.sha256(body).hexdigest(), len(body)


def delete_object(key: str, *, cfg: OssConfig | None = None) -> None:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    try:
        _request("DELETE", key, cfg=config)
    except OssError as exc:
        if "HTTP 404" in str(exc):
            return
        raise


def verify_published(
    kind: str,
    sha256: str,
    size: int,
    *,
    version: str = "",
    cfg: OssConfig | None = None,
) -> dict[str, Any]:
    """Fail unless the live OSS object and manifest match sha256 and size."""
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    spec = _kind_spec(kind, config)
    want = sha256.lower().strip()
    if len(want) != 64 or any(c not in "0123456789abcdef" for c in want):
        raise OssError("invalid sha256")
    got_sha, got_size = object_digest(spec["key"], cfg=config)
    if got_sha != want:
        raise OssError(f"OSS object sha256 mismatch for {spec['key']}")
    if got_size != size:
        raise OssError(f"OSS object size mismatch for {spec['key']}: {got_size} != {size}")
    raw = get_object_text(spec["meta_key"], cfg=config)
    if not raw:
        raise OssError(f"missing manifest {spec['meta_key']}")
    try:
        meta = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise OssError("manifest is not JSON") from exc
    if not isinstance(meta, dict):
        raise OssError("manifest is not an object")
    if str(meta.get("sha256") or "").lower() != want:
        raise OssError("manifest sha256 mismatch")
    try:
        meta_size = int(meta.get("size") or 0)
    except (TypeError, ValueError) as exc:
        raise OssError("manifest size is not an integer") from exc
    if meta_size != size:
        raise OssError("manifest size mismatch")
    if version and str(meta.get("version") or "") != str(version):
        raise OssError("manifest version mismatch")
    return meta


_MULTIPART_THRESHOLD = 8 * 1024 * 1024
_MULTIPART_PART = 8 * 1024 * 1024


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
    if len(data) >= _MULTIPART_THRESHOLD:
        _put_via_socket(key, data, content_type=content_type, meta=meta, cfg=config)
        return
    _request("PUT", key, body=data, content_type=content_type, meta=meta, timeout=300, cfg=config)


def _put_via_socket(
    key: str,
    data: bytes,
    *,
    content_type: str,
    meta: dict[str, str] | None,
    cfg: OssConfig,
    send_chunk: int = 32 * 1024,
    timeout: int = 900,
) -> None:
    """Single PUT over TLS. Sends the body in small chunks, then waits for 200."""
    payload_hash = hashlib.sha256(data).hexdigest()
    headers: dict[str, str] = {"content-type": content_type}
    if meta:
        for name, value in meta.items():
            headers[f"x-amz-meta-{name}"] = value
    uri, _query, signed_headers = _sign_v4(
        method="PUT",
        cfg=cfg,
        key=key,
        now=_utc_now(),
        headers=headers,
        payload_hash=payload_hash,
        presign=False,
    )
    req_headers = {
        "Host": cfg.host,
        "Content-Length": str(len(data)),
        "Connection": "close",
    }
    req_headers.update(signed_headers)
    head = f"PUT {uri} HTTP/1.1\r\n" + "".join(f"{name}: {value}\r\n" for name, value in req_headers.items()) + "\r\n"
    sock: ssl.SSLSocket | None = None
    try:
        raw = socket.create_connection((cfg.host, 443), timeout=30)
        sock = ssl.create_default_context().wrap_socket(raw, server_hostname=cfg.host)
        sock.settimeout(timeout)
        sock.sendall(head.encode("ascii"))
        sent = 0
        while sent < len(data):
            sock.sendall(data[sent : sent + send_chunk])
            sent += send_chunk
        resp = b""
        while True:
            block = sock.recv(4096)
            if not block:
                break
            resp += block
            if b"\r\n\r\n" in resp:
                break
    except OSError as exc:
        raise OssError(f"OSS PUT {key} socket error after timeout={timeout}s: {type(exc).__name__}: {exc}") from exc
    finally:
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass
    if not resp:
        raise OssError(f"OSS PUT {key} failed: empty response after sending {len(data)} bytes")
    status_line = resp.split(b"\r\n", 1)[0].decode("iso-8859-1", errors="replace")
    parts = status_line.split(" ", 2)
    code = int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else 0
    if code != 200:
        raise OssError(f"OSS PUT {key} failed: {status_line} ({len(data)} bytes)")


def _put_file_via_curl(
    key: str,
    *,
    path: Path | None = None,
    data: bytes | None = None,
    content_type: str,
    meta: dict[str, str] | None,
    cfg: OssConfig,
) -> None:
    """Upload large objects with curl so the TCP write is not bound by urllib timeouts."""
    if path is None and data is None:
        raise OssError("upload requires a file path or bytes")
    if path is not None:
        payload_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        upload_path = path
        tmp = None
    else:
        assert data is not None
        payload_hash = hashlib.sha256(data).hexdigest()
        tmp = tempfile.NamedTemporaryFile(prefix="oss-put-", suffix=".bin")
        tmp.write(data)
        tmp.flush()
        upload_path = Path(tmp.name)
    headers: dict[str, str] = {"content-type": content_type}
    if meta:
        for name, value in meta.items():
            headers[f"x-amz-meta-{name}"] = value
    uri, _query, signed_headers = _sign_v4(
        method="PUT",
        cfg=cfg,
        key=key,
        now=_utc_now(),
        headers=headers,
        payload_hash=payload_hash,
        presign=False,
    )
    cmd = [
        "curl",
        "-sS",
        "-f",
        "-X",
        "PUT",
        "--max-time",
        "600",
        "--connect-timeout",
        "30",
        "-o",
        "/dev/null",
        "-w",
        "%{http_code}",
        "-T",
        str(upload_path),
        "-H",
        "Expect:",
        f"{cfg.base_url}{uri}",
    ]
    for name, value in signed_headers.items():
        cmd += ["-H", f"{name}: {value}"]
    try:
        completed = subprocess.run(cmd, check=False, capture_output=True, text=True)
    except FileNotFoundError as exc:
        raise OssError("curl is required to upload large APKs") from exc
    finally:
        if tmp is not None:
            tmp.close()
    if completed.returncode != 0 or not completed.stdout.endswith("200"):
        detail = (completed.stderr or completed.stdout or "curl upload failed")[:400]
        raise OssError(f"OSS PUT {key} failed: {detail}")


def _xml_text(blob: bytes, tag: str) -> str:
    start = blob.find(f"<{tag}>".encode())
    end = blob.find(f"</{tag}>".encode())
    if start < 0 or end < 0:
        raise OssError(f"OSS response missing <{tag}>")
    start += len(tag) + 2
    return blob[start:end].decode("utf-8")


def _put_multipart(
    key: str,
    data: bytes,
    *,
    content_type: str,
    meta: dict[str, str] | None,
    cfg: OssConfig,
) -> None:
    _status, body, _headers = _request(
        "POST",
        key,
        content_type=content_type,
        meta=meta,
        extra_query={"uploads": ""},
        timeout=60,
        cfg=cfg,
    )
    upload_id = _xml_text(body, "UploadId")
    parts: list[tuple[int, str]] = []
    try:
        for index, offset in enumerate(range(0, len(data), _MULTIPART_PART), start=1):
            chunk = data[offset : offset + _MULTIPART_PART]
            _part_status, _part_body, part_headers = _request(
                "PUT",
                key,
                body=chunk,
                extra_query={"partNumber": str(index), "uploadId": upload_id},
                timeout=300,
                cfg=cfg,
            )
            etag = (part_headers.get("etag") or "").strip()
            if not etag:
                raise OssError(f"OSS multipart part {index} missing ETag")
            parts.append((index, etag))
        complete = "<CompleteMultipartUpload>" + "".join(
            f"<Part><PartNumber>{num}</PartNumber><ETag>{etag}</ETag></Part>" for num, etag in parts
        ) + "</CompleteMultipartUpload>"
        _request(
            "POST",
            key,
            body=complete.encode("utf-8"),
            content_type="application/xml",
            extra_query={"uploadId": upload_id},
            timeout=120,
            cfg=cfg,
        )
    except Exception:
        try:
            _request(
                "DELETE",
                key,
                extra_query={"uploadId": upload_id},
                timeout=60,
                cfg=cfg,
            )
        except OssError:
            pass
        raise


def upload_apk(
    path: str | Path,
    *,
    version: str = "",
    version_code: str = "",
    cfg: OssConfig | None = None,
) -> dict[str, Any]:
    return upload_download("android", path, version=version, version_code=version_code, cfg=cfg)


def upload_download(
    kind: str,
    path: str | Path,
    *,
    version: str = "",
    version_code: str = "",
    filename: str = "",
    cfg: OssConfig | None = None,
) -> dict[str, Any]:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    spec = _kind_spec(kind, config)
    src = Path(path)
    if not src.is_file():
        raise OssError(f"package not found: {src}")
    data = src.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    uploaded_at = _utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    download_name = filename or spec["filename"]
    meta = {
        "sha256": digest,
        "filename": download_name,
        "uploaded-at": uploaded_at,
    }
    if version:
        meta["version"] = version
    if version_code:
        meta["version-code"] = str(version_code)
    put_object(
        spec["key"],
        data,
        content_type=spec["content_type"],
        meta=meta,
        cfg=config,
    )
    info = _package_info(spec["key"], download_name, len(data), digest, uploaded_at, version, version_code)
    write_manifest(spec["meta_key"], info, cfg=config)
    verify_published(kind, digest, len(data), version=version, cfg=config)
    if version:
        from remote.server.update import follow_published_version

        follow_published_version(version)
    return info


def _apk_info(
    key: str,
    size: int,
    digest: str,
    uploaded_at: str,
    version: str,
    version_code: str,
) -> dict[str, Any]:
    return _package_info(key, DOWNLOAD_FILENAME, size, digest, uploaded_at, version, version_code)


def _package_info(
    key: str,
    filename: str,
    size: int,
    digest: str,
    uploaded_at: str,
    version: str,
    version_code: str,
) -> dict[str, Any]:
    return {
        "key": key,
        "filename": filename,
        "size": size,
        "sha256": digest,
        "uploaded_at": uploaded_at,
        "version": version or None,
        "version_code": int(version_code) if str(version_code).isdigit() else None,
    }


def write_apk_manifest(info: dict[str, Any], *, cfg: OssConfig | None = None) -> None:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    write_manifest(config.meta_key, info, cfg=config)


def write_manifest(meta_key: str, info: dict[str, Any], *, cfg: OssConfig | None = None) -> None:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    put_object(
        meta_key,
        json.dumps(info, ensure_ascii=False, indent=2).encode("utf-8"),
        content_type="application/json; charset=utf-8",
        cfg=config,
    )


def apk_put_instructions(
    path: str | Path,
    *,
    version: str = "",
    version_code: str = "",
    expires: int = 7200,
    cfg: OssConfig | None = None,
) -> dict[str, Any]:
    """Describe a local-machine PUT: APK stays here, caller uploads via presigned URL."""
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    apk_path = Path(path)
    if not apk_path.is_file():
        raise OssError(f"APK not found: {apk_path}")
    data = apk_path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    uploaded_at = _utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    info = _apk_info(config.apk_key, len(data), digest, uploaded_at, version, version_code)
    write_manifest(config.meta_key, info, cfg=config)
    url = presign_put(config.apk_key, expires=expires, cfg=config)
    return {
        **info,
        "put_url": url,
        "expires_in": expires,
        "curl": f'curl -f -T "{DOWNLOAD_FILENAME}" "{url}"',
    }


def apk_download_payload(*, expires: int = DEFAULT_EXPIRES, cfg: OssConfig | None = None) -> dict[str, Any]:
    return download_payload("android", expires=expires, cfg=cfg)


def download_payload(kind: str, *, expires: int = DEFAULT_EXPIRES, cfg: OssConfig | None = None) -> dict[str, Any]:
    config = cfg or load_config()
    if config is None:
        raise OssError("OSS is not configured")
    spec = _kind_spec(kind, config)
    headers = head_object(spec["key"], cfg=config)
    if headers is None and spec["kind"] == "android" and spec["key"].endswith(".bin"):
        # Older uploads used a .apk key; JD Cloud forbids that on the default domain.
        headers = head_object("downloads/remotedesk-android.apk", cfg=config)
        if headers is not None:
            spec = {**spec, "key": "downloads/remotedesk-android.apk"}
    if headers is None:
        raise OssError(spec["missing"])
    meta: dict[str, Any] = {}
    raw_meta = get_object_text(spec["meta_key"], cfg=config)
    if raw_meta:
        try:
            parsed = json.loads(raw_meta)
            if isinstance(parsed, dict):
                meta = parsed
        except json.JSONDecodeError:
            meta = {}
    filename = versioned_download_name(
        spec["kind"],
        str(meta.get("version") or ""),
        str(meta.get("filename") or spec["filename"]),
    )
    url = presign_get(
        spec["key"],
        expires=expires,
        filename=filename,
        content_type=spec["content_type"],
        cfg=config,
    )
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


def archive_for_upload(path: str | Path, dest_zip: str | Path) -> Path:
    """Zip a .app / folder for OSS. Files are returned unchanged."""
    src = Path(path)
    if src.is_file():
        return src
    if not src.is_dir():
        raise OssError(f"package not found: {src}")
    dest = Path(dest_zip)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        dest.unlink()
    if sys.platform == "darwin":
        subprocess.check_call(["ditto", "-c", "-k", "--keepParent", str(src), str(dest)])
        return dest
    import zipfile

    with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for item in src.rglob("*"):
            if item.is_file():
                zf.write(item, item.relative_to(src.parent))
    return dest


def guess_content_type(path: Path) -> str:
    guessed, _ = mimetypes.guess_type(path.name)
    return guessed or "application/octet-stream"

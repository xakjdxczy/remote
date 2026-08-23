"""Publish Windows DustX to OSS: Mac presigns, Windows PUTs, Mac verifies by GET.

Credentials never leave this Mac. Presigned URLs are passed in a curl config file
and are not printed. Success means the live OSS object hashes to the expected sha256.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from remote.server.oss import (
    DEFAULT_WINDOWS_STAGING_KEY,
    OssError,
    WINDOWS_FILENAME,
    _kind_spec,
    _package_info,
    _utc_now,
    delete_object,
    get_object_bytes,
    load_config,
    presign_put,
    put_object,
    verify_published,
    write_manifest,
)

_HOST_RE = re.compile(r"^[A-Za-z0-9._-]+$")
_NAME_RE = re.compile(r"^[A-Za-z0-9._-]+$")


def _redact(text: str) -> str:
    text = re.sub(r"https://[^\s\"']+", "[redacted-url]", text)
    text = re.sub(r"X-Amz-[A-Za-z0-9]+=[^\s&\"']+", "X-Amz-*=[redacted]", text)
    return text[-800:]


def parse_from_ssh(value: str) -> tuple[str, str]:
    raw = (value or "").strip()
    if not raw:
        raise OssError("missing --from-ssh host:path")
    host, sep, path = raw.partition(":")
    if not sep:
        host, path = raw, WINDOWS_FILENAME
    path = path.strip() or WINDOWS_FILENAME
    if not _HOST_RE.match(host):
        raise OssError("invalid ssh host")
    if "/" in path or "\\" in path:
        raise OssError("remote path must be a file name under the Windows user profile")
    if not _NAME_RE.match(path):
        raise OssError("invalid remote file name")
    return host, path


def _ssh(host: str, command: str, *, timeout: int) -> str:
    proc = subprocess.run(
        [
            "ssh",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            "-o", "ServerAliveInterval=20",
            host,
            command,
        ],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if proc.returncode != 0:
        err = _redact((proc.stderr or proc.stdout or "").strip() or f"exit {proc.returncode}")
        raise OssError(f"ssh {host} failed: {err}")
    return (proc.stdout or "").strip()


def _scp_to(host: str, local: Path, remote_name: str, *, timeout: int = 60) -> None:
    proc = subprocess.run(
        [
            "scp", "-O",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            str(local),
            f"{host}:{remote_name}",
        ],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if proc.returncode != 0:
        raise OssError(f"scp to {host} failed: {_redact(proc.stderr or '')}")


def _scp_from(host: str, remote_name: str, local: Path, *, timeout: int = 180) -> None:
    proc = subprocess.run(
        [
            "scp", "-O",
            "-o", "BatchMode=yes",
            "-o", "ConnectTimeout=10",
            f"{host}:{remote_name}",
            str(local),
        ],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if proc.returncode != 0:
        raise OssError(f"scp from {host} failed: {_redact(proc.stderr or '')}")


def remote_file_digest(host: str, filename: str) -> tuple[str, int, str]:
    ps = (
        "$ErrorActionPreference='Stop'; "
        f"$p=Join-Path $env:USERPROFILE '{filename}'; "
        "if (-not (Test-Path -LiteralPath $p)) { throw 'missing zip' }; "
        "$h=Get-FileHash -LiteralPath $p -Algorithm SHA256; "
        "$n=(Get-Item -LiteralPath $p).Length; "
        "Write-Output ($h.Hash.ToLower() + '|' + $n + '|' + $p)"
    )
    out = _ssh(host, f"powershell.exe -NoProfile -NonInteractive -Command \"{ps}\"", timeout=60)
    parts = out.split("|", 2)
    if len(parts) != 3:
        raise OssError("could not read remote zip hash")
    digest, size_s, win_path = parts[0].lower().strip(), parts[1].strip(), parts[2].strip()
    if len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        raise OssError("remote sha256 is not a hex digest")
    size = int(size_s)
    if size <= 0:
        raise OssError("remote zip is empty")
    if not win_path:
        raise OssError("remote zip path is empty")
    return digest, size, win_path


def _windows_put(host: str, win_path: str, put_url: str, *, timeout: int = 600) -> None:
    """PUT via curl -K so the presigned URL is not on the ssh command line."""
    upload = win_path.replace("\\", "/")
    safe_url = put_url.replace("\\", "\\\\").replace('"', '\\"')
    cfg = tempfile.NamedTemporaryFile("w", prefix="dustx-oss-put-", suffix=".cfg", delete=False)
    try:
        os.chmod(cfg.name, 0o600)
        cfg.write(
            "fail = true\n"
            "silent = true\n"
            "show-error = true\n"
            "request = PUT\n"
            f'url = "{safe_url}"\n'
            'header = "Content-Type: application/zip"\n'
            "max-time = 600\n"
            "output = NUL\n"
            'write-out = "%{http_code}\\n"\n'
            f'upload-file = "{upload}"\n'
        )
        cfg.close()
        remote_cfg = "dustx-oss-put.cfg"
        _scp_to(host, Path(cfg.name), remote_cfg, timeout=30)
        try:
            # cmd.exe so %USERPROFILE% expands even if OpenSSH's default shell is PowerShell.
            code = _ssh(
                host,
                'cmd.exe /c "curl.exe -K %USERPROFILE%\\dustx-oss-put.cfg & '
                'del /q %USERPROFILE%\\dustx-oss-put.cfg"',
                timeout=timeout,
            )
        except OssError:
            try:
                _ssh(host, 'cmd.exe /c "del /q %USERPROFILE%\\dustx-oss-put.cfg"', timeout=15)
            except OssError:
                pass
            raise
        last = code.strip().split()[-1] if code.strip() else ""
        if last not in {"200", "204"}:
            raise OssError(f"Windows PUT returned HTTP {code.strip() or 'empty'}")
    finally:
        try:
            os.unlink(cfg.name)
        except OSError:
            pass


def _promote_staging(digest: str, size: int, version: str, version_code: str, cfg) -> dict[str, Any]:
    spec = _kind_spec("windows", cfg)
    body = get_object_bytes(DEFAULT_WINDOWS_STAGING_KEY, cfg=cfg)
    if hashlib.sha256(body).hexdigest() != digest or len(body) != size:
        raise OssError("staging object does not match the Windows zip hash")
    uploaded_at = _utc_now().strftime("%Y-%m-%dT%H:%M:%SZ")
    meta = {
        "sha256": digest,
        "filename": spec["filename"],
        "uploaded-at": uploaded_at,
    }
    if version:
        meta["version"] = version
    if version_code:
        meta["version-code"] = str(version_code)
    put_object(spec["key"], body, content_type=spec["content_type"], meta=meta, cfg=cfg)
    info = _package_info(spec["key"], spec["filename"], size, digest, uploaded_at, version, version_code)
    write_manifest(spec["meta_key"], info, cfg=cfg)
    verify_published("windows", digest, size, version=version, cfg=cfg)
    if version:
        from remote.server.update import follow_published_version

        follow_published_version(version)
    try:
        delete_object(DEFAULT_WINDOWS_STAGING_KEY, cfg=cfg)
    except OssError:
        pass
    return info


def publish_windows_from_ssh(
    from_ssh: str,
    *,
    version: str = "",
    version_code: str = "",
    expires: int = 7200,
) -> dict[str, Any]:
    host, filename = parse_from_ssh(from_ssh)
    cfg = load_config()
    if cfg is None:
        raise OssError("OSS is not configured")
    digest, size, win_path = remote_file_digest(host, filename)
    via = "windows-put"
    try:
        put_url = presign_put(DEFAULT_WINDOWS_STAGING_KEY, expires=expires, cfg=cfg)
        _windows_put(host, win_path, put_url)
        info = _promote_staging(digest, size, version, version_code, cfg)
    except OssError as first:
        via = "mac-fallback"
        tmp = Path(tempfile.mkdtemp(prefix="dustx-win-")) / filename
        try:
            try:
                _scp_from(host, filename, tmp, timeout=max(120, size // (50_000) + 60))
            except OssError as copy_err:
                raise OssError(
                    f"Windows PUT failed ({first}); copy fallback failed ({copy_err})"
                ) from first
            local_digest = hashlib.sha256(tmp.read_bytes()).hexdigest()
            if local_digest != digest or tmp.stat().st_size != size:
                raise OssError("copied zip does not match the Windows hash") from first
            from remote.server.oss import upload_download

            info = upload_download("windows", tmp, version=version, version_code=version_code, cfg=cfg)
        finally:
            try:
                tmp.unlink(missing_ok=True)
                tmp.parent.rmdir()
            except OSError:
                pass
        info = {**info, "fallback_error": str(first)}
    info = {**info, "via": via, "verified": True}
    return info

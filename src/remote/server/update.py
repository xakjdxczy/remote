"""Desktop update policy: latest OSS package plus optional forced rollout."""

from __future__ import annotations

import json
import logging
import os
import threading
import time
from pathlib import Path
from typing import Any

from remote.server import oss
from remote.urls import official_http
from remote.versioning import version_less

logger = logging.getLogger("remotedesk.server")

_memory: dict[str, Any] | None = None
_lock = threading.Lock()


def _use_memory() -> bool:
    raw = (os.environ.get("DUSTX_UPDATE_POLICY") or "").strip().lower()
    return raw in {"memory", "none", "off"}


def policy_path() -> Path | None:
    if _use_memory():
        return None
    raw = (os.environ.get("DUSTX_UPDATE_POLICY") or "").strip()
    if raw:
        return Path(raw)
    data = (os.environ.get("DUSTX_DATA_DIR") or "").strip()
    root = Path(data) if data else Path.home() / ".dustx"
    return root / "update_policy.json"


def load_policy() -> dict[str, Any]:
    if _use_memory():
        with _lock:
            return dict(_memory or {})
    path = policy_path()
    if path is None or not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def follow_published_version(version: str) -> None:
    """If force-update is on, raise the floor to the package that just went live."""
    ver = str(version or "").strip()
    if not ver:
        return
    policy = load_policy()
    if not policy.get("force"):
        return
    current = str(policy.get("version") or "")
    if current and not version_less(current, ver):
        return
    save_policy(force=True, version=ver, notes=str(policy.get("notes") or ""))


def save_policy(*, force: bool, version: str = "", notes: str = "") -> dict[str, Any]:
    policy = {
        "force": bool(force),
        "version": str(version or "").strip(),
        "notes": str(notes or "").strip(),
        "updated_at": int(time.time()),
    }
    if _use_memory():
        with _lock:
            global _memory
            _memory = dict(policy)
        return policy
    path = policy_path()
    if path is None:
        return policy
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(policy, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tmp.replace(path)
    logger.info("update policy force=%s version=%s", policy["force"], policy["version"] or "-")
    return policy


def policy_stamp() -> tuple[bool, str, int]:
    policy = load_policy()
    return bool(policy.get("force")), str(policy.get("version") or ""), int(policy.get("updated_at") or 0)


def update_hint(current: str = "") -> dict[str, Any]:
    """WS payload. ``force`` means this client is behind and must update now."""
    policy = load_policy()
    required = str(policy.get("version") or "")
    policy_on = bool(policy.get("force"))
    must = bool(policy_on and current and required and version_less(current, required))
    return {
        "force": must,
        "policy": policy_on,
        "required": required,
        "notes": str(policy.get("notes") or ""),
        "current": current or None,
    }


def update_payload(platform: str, current: str = "") -> dict[str, Any]:
    kind = "macos" if str(platform or "").lower() in {"macos", "darwin", "mac"} else "windows"
    policy = load_policy()
    hint = update_hint(current)
    payload: dict[str, Any] = {
        "ok": False,
        "platform": kind,
        "current": current or None,
        "latest": None,
        "required": hint["required"] or None,
        "force": hint["force"],
        "newer": False,
        "notes": hint["notes"],
    }
    try:
        pkg = oss.download_payload(kind)
    except oss.OssError as exc:
        payload["error"] = str(exc)
        payload["force"] = False
        return payload
    latest = str(pkg.get("version") or "")
    required = hint["required"] or latest
    newer = bool(current and latest and version_less(current, latest))
    # A stale required (e.g. still 6 after 8 was published) must not block force.
    package_ready = bool(
        pkg.get("url")
        and pkg.get("size")
        and (not hint["required"] or not version_less(latest, hint["required"]))
    )
    must = bool(policy.get("force") and newer and package_ready)
    sha = pkg.get("sha256")
    # Windows clients before 2026.8.21.9 parse `certutil` as "256" and always fail checksum.
    if kind == "windows" and (not current or version_less(current, "2026.8.21.9")):
        sha = None
    payload.update(
        {
            "ok": True,
            "latest": latest or None,
            "required": required or None,
            "newer": newer,
            "force": must,
            "notes": hint["notes"] if newer else "",
            "url": f"{official_http()}/api/update/file?platform={kind}",
            "filename": pkg.get("filename"),
            "sha256": sha,
            "size": pkg.get("size"),
        }
    )
    return payload

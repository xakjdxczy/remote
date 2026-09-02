"""Click counts and a daily byte budget for official package downloads.

Version-preview GETs do not increment. Clicks, redirects, and updater fetches do.
The day rolls over at midnight Asia/Shanghai. Default cap is 10 GiB.
"""

from __future__ import annotations

import json
import logging
import os
import threading
from datetime import datetime
from pathlib import Path
from urllib.parse import unquote, urlparse
from zoneinfo import ZoneInfo

from remote.server.oss import DOWNLOAD_KINDS

CLICK_KINDS = DOWNLOAD_KINDS + ("hosted",)
HOSTED_EXTS = {".apk", ".zip", ".exe", ".dmg", ".bin"}

logger = logging.getLogger("remotedesk.server")

SHANGHAI = ZoneInfo("Asia/Shanghai")
DEFAULT_DAILY_BYTES = 10 * 1024 * 1024 * 1024
BUSY_MESSAGE = "今日下载通道繁忙，请明天再试。"

_lock = threading.Lock()


def _path() -> Path | None:
    raw = (os.environ.get("DUSTX_DOWNLOAD_STATS") or "").strip()
    if raw.lower() in {"memory", "none", "off"}:
        return None
    if raw:
        return Path(raw)
    data = (os.environ.get("DUSTX_DATA_DIR") or "").strip()
    root = Path(data) if data else Path.home() / ".dustx"
    return root / "download_clicks.json"


def _today() -> str:
    return datetime.now(SHANGHAI).strftime("%Y-%m-%d")


def daily_limit() -> int:
    raw = (os.environ.get("DUSTX_DOWNLOAD_DAILY_BYTES") or "").strip()
    if not raw:
        return DEFAULT_DAILY_BYTES
    try:
        n = int(raw)
    except ValueError:
        return DEFAULT_DAILY_BYTES
    return n if n > 0 else DEFAULT_DAILY_BYTES


def _empty_clicks() -> dict[str, int]:
    return {kind: 0 for kind in CLICK_KINDS}


def _read() -> dict:
    path = _path()
    out: dict = {**_empty_clicks(), "day": _today(), "bytes": 0}
    if path is None or not path.is_file():
        return out
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return out
    if not isinstance(data, dict):
        return out
    for kind in CLICK_KINDS:
        try:
            out[kind] = int(data.get(kind) or 0)
        except (TypeError, ValueError):
            out[kind] = 0
    day = str(data.get("day") or "")
    if day == _today():
        out["day"] = day
        try:
            out["bytes"] = max(0, int(data.get("bytes") or 0))
        except (TypeError, ValueError):
            out["bytes"] = 0
    return out


def _write(state: dict) -> None:
    path = _path()
    if path is None:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {kind: int(state.get(kind) or 0) for kind in CLICK_KINDS}
    payload["day"] = str(state.get("day") or _today())
    payload["bytes"] = max(0, int(state.get("bytes") or 0))
    tmp = path.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(payload, ensure_ascii=False, indent=0) + "\n", encoding="utf-8")
    tmp.replace(path)


def counts() -> dict[str, int]:
    with _lock:
        st = _read()
        return {kind: int(st.get(kind) or 0) for kind in CLICK_KINDS}


def quota_full() -> bool:
    with _lock:
        return int(_read().get("bytes") or 0) >= daily_limit()


def www_root() -> Path:
    raw = (os.environ.get("DUSTX_WWW_ROOT") or "").strip()
    return Path(raw) if raw else Path("/var/www")


def hosted_file(uri: str) -> Path | None:
    """Map a public path like /board/android/foo.apk onto a file under www root."""
    path = unquote(urlparse(uri or "").path or "")
    if not path or ".." in path:
        return None
    if Path(path).suffix.lower() not in HOSTED_EXTS:
        return None
    if path.startswith("/board/"):
        full = (www_root() / path.lstrip("/")).resolve()
        base = (www_root() / "board").resolve()
    elif path.startswith("/study/"):
        full = (www_root() / "study" / path[len("/study/") :].lstrip("/")).resolve()
        base = (www_root() / "study").resolve()
    else:
        return None
    try:
        full.relative_to(base)
    except ValueError:
        return None
    if not full.is_file():
        return None
    return full


def record_click(kind: str, size: int = 0) -> None:
    name = (kind or "").strip().lower()
    if name not in CLICK_KINDS:
        name = "hosted"
    path = _path()
    if path is None:
        return
    extra = max(0, int(size or 0))
    with _lock:
        st = _read()
        st[name] = int(st.get(name) or 0) + 1
        st["bytes"] = int(st.get("bytes") or 0) + extra
        st["day"] = _today()
        _write(st)
        logger.info(
            "download click kind=%s total=%s day_bytes=%s limit=%s",
            name,
            st[name],
            st["bytes"],
            daily_limit(),
        )

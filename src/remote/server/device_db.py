"""SQLite table: motherboard + NIC + system UUID → remote code."""

from __future__ import annotations

import json
import logging
import os
import sqlite3
import threading
import time
import uuid
from pathlib import Path

from remote.device_fp import HardwareFingerprint

logger = logging.getLogger("remotedesk.server")


def resolve_device_db_path() -> Path | None:
    raw = (os.environ.get("DUSTX_DEVICE_DB") or "").strip()
    if raw.lower() in {"memory", "none", "off"}:
        return None
    if raw:
        return Path(raw)
    data = (os.environ.get("DUSTX_DATA_DIR") or "").strip()
    root = Path(data) if data else Path.home() / ".dustx"
    return root / "devices.sqlite"


class DeviceDB:
    def __init__(self, path: Path | str | None) -> None:
        self.path = Path(path) if path else None
        target = str(self.path) if self.path else ":memory:"
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(target, check_same_thread=False)
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=FULL")
        self._conn.execute(
            """
            CREATE TABLE IF NOT EXISTS devices (
                fingerprint TEXT PRIMARY KEY,
                device_id TEXT NOT NULL UNIQUE,
                board TEXT NOT NULL,
                nic TEXT NOT NULL,
                sys_uuid TEXT NOT NULL,
                created_at REAL NOT NULL,
                last_seen REAL NOT NULL
            )
            """
        )
        self._conn.execute(
            """
            CREATE TABLE IF NOT EXISTS device_info (
                device_id TEXT PRIMARY KEY,
                info_json TEXT NOT NULL,
                last_ip TEXT NOT NULL DEFAULT '',
                updated_at REAL NOT NULL
            )
            """
        )
        self._conn.execute(
            """
            CREATE TABLE IF NOT EXISTS device_passwords (
                device_id TEXT PRIMARY KEY,
                password TEXT NOT NULL,
                updated_at REAL NOT NULL
            )
            """
        )
        self._conn.execute(
            """
            CREATE TABLE IF NOT EXISTS device_pairs (
                viewer_id TEXT NOT NULL,
                host_id TEXT NOT NULL,
                token TEXT NOT NULL,
                created_at REAL NOT NULL,
                PRIMARY KEY (viewer_id, host_id)
            )
            """
        )
        self._conn.commit()

    def device_id_for(self, fp: HardwareFingerprint) -> str | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT device_id FROM devices WHERE fingerprint = ?",
                (fp.key,),
            ).fetchone()
        return row[0] if row else None

    def fingerprint_for(self, device_id: str) -> str | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT fingerprint FROM devices WHERE device_id = ?",
                (device_id,),
            ).fetchone()
        return row[0] if row else None

    def bind(self, fp: HardwareFingerprint, device_id: str) -> None:
        now = time.time()
        with self._lock:
            self._conn.execute(
                """
                INSERT INTO devices (fingerprint, device_id, board, nic, sys_uuid, created_at, last_seen)
                VALUES (?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(fingerprint) DO UPDATE SET
                    last_seen = excluded.last_seen
                """,
                (fp.key, device_id, fp.board, fp.nic, fp.sys_uuid, now, now),
            )
            self._conn.commit()

    def touch(self, fp: HardwareFingerprint) -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE devices SET last_seen = ? WHERE fingerprint = ?",
                (time.time(), fp.key),
            )
            self._conn.commit()

    def all_device_ids(self) -> set[str]:
        with self._lock:
            rows = self._conn.execute("SELECT device_id FROM devices").fetchall()
        return {row[0] for row in rows}

    def count(self) -> int:
        with self._lock:
            row = self._conn.execute("SELECT COUNT(*) FROM devices").fetchone()
        return int(row[0]) if row else 0

    def save_info(self, device_id: str, info: dict, last_ip: str = "") -> None:
        did = str(device_id or "")
        if len(did) != 9:
            return
        payload = json.dumps(info or {}, ensure_ascii=False)
        now = time.time()
        with self._lock:
            self._conn.execute(
                """
                INSERT INTO device_info (device_id, info_json, last_ip, updated_at)
                VALUES (?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    info_json = excluded.info_json,
                    last_ip = CASE WHEN excluded.last_ip = '' THEN device_info.last_ip ELSE excluded.last_ip END,
                    updated_at = excluded.updated_at
                """,
                (did, payload, last_ip or "", now),
            )
            self._conn.commit()

    def get_password(self, device_id: str) -> str | None:
        did = str(device_id or "")
        if len(did) != 9:
            return None
        with self._lock:
            row = self._conn.execute(
                "SELECT password FROM device_passwords WHERE device_id = ?",
                (did,),
            ).fetchone()
        pw = str(row[0] or "") if row else ""
        return pw or None

    def set_password(self, device_id: str, password: str) -> None:
        did = str(device_id or "")
        pw = str(password or "")
        if len(did) != 9 or not pw:
            return
        now = time.time()
        with self._lock:
            self._conn.execute(
                """
                INSERT INTO device_passwords (device_id, password, updated_at)
                VALUES (?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    password = excluded.password,
                    updated_at = excluded.updated_at
                """,
                (did, pw, now),
            )
            self._conn.commit()

    def load_info(self, device_id: str) -> dict | None:
        did = str(device_id or "")
        if len(did) != 9:
            return None
        with self._lock:
            row = self._conn.execute(
                "SELECT info_json, last_ip, updated_at FROM device_info WHERE device_id = ?",
                (did,),
            ).fetchone()
        if not row:
            return None
        try:
            info = json.loads(row[0] or "{}")
        except json.JSONDecodeError:
            info = {}
        if not isinstance(info, dict):
            info = {}
        return {"info": info, "last_ip": row[1] or "", "updated_at": float(row[2] or 0)}

    def upsert_pair(self, viewer_id: str, host_id: str, token: str = "") -> str:
        vid = str(viewer_id or "")
        hid = str(host_id or "")
        if len(vid) != 9 or len(hid) != 9 or vid == hid:
            return ""
        now = time.time()
        with self._lock:
            row = self._conn.execute(
                "SELECT token FROM device_pairs WHERE viewer_id = ? AND host_id = ?",
                (vid, hid),
            ).fetchone()
            if row and row[0]:
                return str(row[0])
            tok = str(token or "") or uuid.uuid4().hex
            self._conn.execute(
                """
                INSERT INTO device_pairs (viewer_id, host_id, token, created_at)
                VALUES (?, ?, ?, ?)
                """,
                (vid, hid, tok, now),
            )
            self._conn.commit()
            return tok

    def pair_token(self, viewer_id: str, host_id: str) -> str:
        vid = str(viewer_id or "")
        hid = str(host_id or "")
        if len(vid) != 9 or len(hid) != 9:
            return ""
        with self._lock:
            row = self._conn.execute(
                "SELECT token FROM device_pairs WHERE viewer_id = ? AND host_id = ?",
                (vid, hid),
            ).fetchone()
        return str(row[0] or "") if row else ""

    def delete_pair(self, left: str, right: str) -> None:
        a = str(left or "")
        b = str(right or "")
        if len(a) != 9 or len(b) != 9:
            return
        with self._lock:
            self._conn.execute(
                "DELETE FROM device_pairs WHERE (viewer_id = ? AND host_id = ?) OR (viewer_id = ? AND host_id = ?)",
                (a, b, b, a),
            )
            self._conn.commit()

    def list_pairs(self, device_id: str) -> list[dict]:
        did = str(device_id or "")
        if len(did) != 9:
            return []
        with self._lock:
            rows = self._conn.execute(
                "SELECT viewer_id, host_id, token, created_at FROM device_pairs WHERE viewer_id = ? OR host_id = ?",
                (did, did),
            ).fetchall()
        out = []
        for viewer_id, host_id, token, created_at in rows:
            role = "viewer" if viewer_id == did else "host"
            peer = host_id if role == "viewer" else viewer_id
            out.append({
                "id": peer,
                "token": token if role == "viewer" else "",
                "role": role,
                "created_at": float(created_at or 0),
            })
        return out


def attach_device_db(registry: object) -> None:
    path = resolve_device_db_path()
    if path is None:
        return
    if getattr(registry, "hw", None) is not None:
        return
    db = DeviceDB(path)
    registry.hw = db  # type: ignore[attr-defined]
    ids = getattr(registry, "ids", None)
    if ids is not None and hasattr(ids, "attach_existing"):
        ids.attach_existing(db.all_device_ids())
    logger.info("device table %s (%d machines)", path, db.count())

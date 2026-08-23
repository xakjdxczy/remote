"""In-memory device and session registry for the relay."""

from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Literal

from remote.device_fp import parse_fingerprint
from remote.ids import generate_temp_password, is_usable_temp_password, normalize_device_id
from remote.server.device_db import DeviceDB
from remote.server.id_store import AllocatedIds


Role = Literal["host", "viewer"]


def is_mesh_label(name: str | None) -> bool:
    return "mesh" in (name or "").lower()


@dataclass
class Device:
    device_id: str
    hostname: str
    os_name: str
    temp_password: str
    ws: Any
    connected_at: float = field(default_factory=time.time)
    last_seen: float = field(default_factory=time.time)
    session_id: str | None = None
    session_ids: list[str] = field(default_factory=list)
    info: dict[str, Any] = field(default_factory=dict)
    last_ip: str = ""


@dataclass
class Session:
    session_id: str
    host_id: str
    viewer_ws: Any
    host_ws: Any
    viewer_name: str
    viewer_id: str | None = None
    created_at: float = field(default_factory=time.time)
    accepted: bool = False


class Registry:
    def __init__(self, ids: AllocatedIds | None = None, hw: DeviceDB | None = None) -> None:
        self.devices: dict[str, Device] = {}
        self.sessions: dict[str, Session] = {}
        self._ws_index: dict[int, str] = {}
        self._passwords: dict[str, str] = {}
        self._password_refreshed_at: dict[str, float] = {}
        self._pairs: dict[tuple[str, str], str] = {}
        self.total_sessions = 0
        self.ids = ids or AllocatedIds()
        self.hw = hw

    def peek_device_id(self, preferred_id: str | None = None, fingerprint: Any = None) -> str | None:
        """Online id this register would claim, if already known."""
        fp = parse_fingerprint(fingerprint)
        if fp and self.hw:
            found = self.hw.device_id_for(fp)
            if found:
                return found
        device_id = normalize_device_id(preferred_id or "")
        if device_id and len(device_id) == 9 and device_id != "000000000":
            if fp and self.hw:
                owner = self.hw.fingerprint_for(device_id)
                if owner and owner != fp.key:
                    return None
            return device_id
        return None

    def _choose_device_id(self, preferred_id: str | None, fingerprint: Any) -> str:
        fp = parse_fingerprint(fingerprint)
        if fp and self.hw:
            found = self.hw.device_id_for(fp)
            if found:
                self.ids.reserve(found)
                self.hw.touch(fp)
                return found
        preferred = normalize_device_id(preferred_id or "")
        if preferred and len(preferred) == 9 and preferred != "000000000":
            if fp and self.hw:
                owner = self.hw.fingerprint_for(preferred)
                if owner and owner != fp.key:
                    device_id = self.ids.allocate(set(self.devices) | self.hw.all_device_ids())
                    self.hw.bind(fp, device_id)
                    return device_id
            self.ids.reserve(preferred)
            if fp and self.hw:
                self.hw.bind(fp, preferred)
            return preferred
        extra = set(self.devices)
        if self.hw:
            extra |= self.hw.all_device_ids()
        device_id = self.ids.allocate(extra)
        if fp and self.hw:
            self.hw.bind(fp, device_id)
        return device_id

    def _stored_password(self, device_id: str) -> str:
        existing = self.devices.get(device_id)
        if existing and is_usable_temp_password(existing.temp_password):
            return existing.temp_password
        mem = self._passwords.get(device_id) or ""
        if is_usable_temp_password(mem):
            return mem
        if self.hw and hasattr(self.hw, "get_password"):
            disk = self.hw.get_password(device_id) or ""
            if is_usable_temp_password(disk):
                self._passwords[device_id] = disk
                return disk
        return ""

    def _save_password(self, device_id: str, password: str) -> None:
        if not is_usable_temp_password(password):
            return
        self._passwords[device_id] = password
        if self.hw and hasattr(self.hw, "set_password"):
            self.hw.set_password(device_id, password)

    def register_host(
        self,
        ws: Any,
        hostname: str,
        os_name: str,
        preferred_id: str | None = None,
        temp_password: str | None = None,
        fingerprint: Any = None,
        info: dict[str, Any] | None = None,
        last_ip: str = "",
    ) -> Device:
        device_id = self._choose_device_id(preferred_id, fingerprint)
        existing = self.devices.get(device_id)
        stored = self._stored_password(device_id)
        if existing and existing.ws is not ws:
            self.unregister(existing.ws)
        if stored:
            password = stored
        elif is_usable_temp_password(temp_password):
            password = temp_password
        else:
            password = generate_temp_password()
        self._save_password(device_id, password)
        device = Device(
            device_id=device_id,
            hostname=hostname or "unknown",
            os_name=os_name or "unknown",
            temp_password=password,
            ws=ws,
            info=dict(info or {}),
            last_ip=last_ip or "",
        )
        self.devices[device_id] = device
        self._ws_index[id(ws)] = device_id
        return device

    def refresh_password(self, device_id: str) -> str | None:
        device = self.devices.get(normalize_device_id(device_id))
        if not device:
            return None
        now = time.time()
        last = self._password_refreshed_at.get(device.device_id, 0)
        if now - last < 30 and is_usable_temp_password(device.temp_password):
            return device.temp_password
        device.temp_password = generate_temp_password()
        self._save_password(device.device_id, device.temp_password)
        self._password_refreshed_at[device.device_id] = now
        return device.temp_password

    def set_password(self, device_id: str, password: str) -> bool:
        device = self.devices.get(normalize_device_id(device_id))
        if not device or not is_usable_temp_password(password):
            return False
        device.temp_password = password
        self._save_password(device.device_id, password)
        return True

    def get(self, device_id: str) -> Device | None:
        return self.devices.get(normalize_device_id(device_id))

    def lookup_by_ws(self, ws: Any) -> Device | None:
        device_id = self._ws_index.get(id(ws))
        return self.devices.get(device_id) if device_id else None

    def touch(self, device_id: str) -> None:
        device = self.get(device_id)
        if device:
            device.last_seen = time.time()

    def unregister(self, ws: Any) -> list[Session]:
        device_id = self._ws_index.pop(id(ws), None)
        dropped: list[Session] = []
        for session in list(self.sessions.values()):
            if session.viewer_ws is ws or session.host_ws is ws:
                ended = self.end_session(session.session_id)
                if ended:
                    dropped.append(ended)
        if device_id:
            device = self.devices.get(device_id)
            if device and self.hw and hasattr(self.hw, "save_info"):
                try:
                    self.hw.save_info(device_id, device.info, device.last_ip)
                except Exception:
                    pass
            self.devices.pop(device_id, None)
        return dropped

    def create_session(
        self,
        host: Device,
        viewer_ws: Any,
        viewer_name: str,
        viewer_id: str | None = None,
    ) -> Session:
        mesh_call = is_mesh_label(viewer_name)
        mesh_count = 0
        remote_count = 0
        for sid in host.session_ids:
            existing = self.sessions.get(sid)
            if not existing:
                continue
            if is_mesh_label(existing.viewer_name):
                mesh_count += 1
            else:
                remote_count += 1
        if mesh_call:
            if mesh_count >= 8:
                raise RuntimeError("busy")
        elif remote_count >= 1:
            raise RuntimeError("busy")
        for existing in self.sessions_for_ws(viewer_ws):
            if existing.viewer_ws is viewer_ws and not mesh_call:
                raise RuntimeError("busy")
        session = Session(
            session_id=uuid.uuid4().hex,
            host_id=host.device_id,
            viewer_ws=viewer_ws,
            host_ws=host.ws,
            viewer_name=viewer_name or "viewer",
            viewer_id=viewer_id,
        )
        self.sessions[session.session_id] = session
        host.session_ids.append(session.session_id)
        host.session_id = session.session_id
        self.total_sessions += 1
        return session

    def get_session(self, session_id: str) -> Session | None:
        return self.sessions.get(session_id)

    def session_for_ws(self, ws: Any) -> Session | None:
        found = self.sessions_for_ws(ws)
        return found[0] if found else None

    def sessions_for_ws(self, ws: Any) -> list[Session]:
        return [s for s in self.sessions.values() if s.viewer_ws is ws or s.host_ws is ws]

    def resolve_session(self, ws: Any, session_id: str | None) -> Session | None:
        sid = str(session_id or "")
        if sid:
            session = self.sessions.get(sid)
            if session and (session.viewer_ws is ws or session.host_ws is ws):
                return session
            return None
        found = self.sessions_for_ws(ws)
        return found[0] if len(found) == 1 else None

    def accept(self, session_id: str) -> Session | None:
        session = self.sessions.get(session_id)
        if session:
            session.accepted = True
        return session

    def end_session(self, session_id: str) -> Session | None:
        session = self.sessions.pop(session_id, None)
        if not session:
            return None
        host = self.devices.get(session.host_id)
        if host:
            host.session_ids = [sid for sid in host.session_ids if sid != session_id]
            host.session_id = host.session_ids[-1] if host.session_ids else None
        return session

    def pair(self, viewer_id: str, host_id: str) -> str:
        vid = normalize_device_id(viewer_id)
        hid = normalize_device_id(host_id)
        if len(vid) != 9 or len(hid) != 9 or vid == hid:
            return ""
        if self.hw and hasattr(self.hw, "upsert_pair"):
            tok = self.hw.upsert_pair(vid, hid)
            if tok:
                return tok
        key = (vid, hid)
        if key not in self._pairs:
            self._pairs[key] = uuid.uuid4().hex
        return self._pairs[key]

    def unpair(self, left: str, right: str) -> None:
        a = normalize_device_id(left)
        b = normalize_device_id(right)
        if self.hw and hasattr(self.hw, "delete_pair"):
            self.hw.delete_pair(a, b)
        self._pairs.pop((a, b), None)
        self._pairs.pop((b, a), None)

    def pair_token(self, viewer_id: str, host_id: str) -> str:
        vid = normalize_device_id(viewer_id)
        hid = normalize_device_id(host_id)
        if self.hw and hasattr(self.hw, "pair_token"):
            tok = self.hw.pair_token(vid, hid)
            if tok:
                return tok
        return self._pairs.get((vid, hid), "")

    def is_paired(self, viewer_id: str, host_id: str) -> bool:
        return bool(self.pair_token(viewer_id, host_id))

    def pair_ok(self, viewer_id: str, host_id: str, token: str) -> bool:
        got = self.pair_token(viewer_id, host_id)
        return bool(got and token and got == token)

    def list_pairs(self, device_id: str) -> list[dict]:
        did = normalize_device_id(device_id)
        if self.hw and hasattr(self.hw, "list_pairs"):
            rows = self.hw.list_pairs(did)
            if rows or not self._pairs:
                return rows
        out = []
        for (vid, hid), token in self._pairs.items():
            if vid == did:
                out.append({"id": hid, "token": token, "role": "viewer", "created_at": 0})
            elif hid == did:
                out.append({"id": vid, "token": "", "role": "host", "created_at": 0})
        return out

    def stats(self) -> dict[str, int]:
        return {
            "hosts": len(self.devices),
            "sessions": len(self.sessions),
            "total_sessions": self.total_sessions,
            "issued": self.ids.count(),
            "machines": self.hw.count() if self.hw else 0,
        }

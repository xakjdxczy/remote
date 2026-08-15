"""In-memory device and session registry for the relay."""

from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Literal

from remote.ids import generate_device_id, generate_temp_password, normalize_device_id


Role = Literal["host", "viewer"]


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


@dataclass
class Session:
    session_id: str
    host_id: str
    viewer_ws: Any
    host_ws: Any
    viewer_name: str
    created_at: float = field(default_factory=time.time)
    accepted: bool = False


class Registry:
    def __init__(self) -> None:
        self.devices: dict[str, Device] = {}
        self.sessions: dict[str, Session] = {}
        self._ws_index: dict[int, str] = {}

    def register_host(
        self,
        ws: Any,
        hostname: str,
        os_name: str,
        preferred_id: str | None = None,
        temp_password: str | None = None,
    ) -> Device:
        device_id = normalize_device_id(preferred_id or "")
        if device_id and len(device_id) == 9 and device_id not in self.devices:
            pass
        else:
            device_id = generate_device_id(set(self.devices))
        password = temp_password or generate_temp_password()
        device = Device(
            device_id=device_id,
            hostname=hostname or "unknown",
            os_name=os_name or "unknown",
            temp_password=password,
            ws=ws,
        )
        self.devices[device_id] = device
        self._ws_index[id(ws)] = device_id
        return device

    def refresh_password(self, device_id: str) -> str | None:
        device = self.devices.get(normalize_device_id(device_id))
        if not device:
            return None
        device.temp_password = generate_temp_password()
        return device.temp_password

    def set_password(self, device_id: str, password: str) -> bool:
        device = self.devices.get(normalize_device_id(device_id))
        if not device or not password:
            return False
        device.temp_password = password
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
        if not device_id:
            # Might be a viewer socket
            for session in list(self.sessions.values()):
                if session.viewer_ws is ws or session.host_ws is ws:
                    dropped.append(self.end_session(session.session_id))
            return [s for s in dropped if s]

        device = self.devices.pop(device_id, None)
        if device and device.session_id:
            ended = self.end_session(device.session_id)
            if ended:
                dropped.append(ended)
        return dropped

    def create_session(self, host: Device, viewer_ws: Any, viewer_name: str) -> Session:
        if host.session_id and host.session_id in self.sessions:
            raise RuntimeError("busy")
        session = Session(
            session_id=uuid.uuid4().hex,
            host_id=host.device_id,
            viewer_ws=viewer_ws,
            host_ws=host.ws,
            viewer_name=viewer_name or "viewer",
        )
        self.sessions[session.session_id] = session
        host.session_id = session.session_id
        return session

    def get_session(self, session_id: str) -> Session | None:
        return self.sessions.get(session_id)

    def session_for_ws(self, ws: Any) -> Session | None:
        for session in self.sessions.values():
            if session.viewer_ws is ws or session.host_ws is ws:
                return session
        return None

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
        if host and host.session_id == session_id:
            host.session_id = None
        return session

    def stats(self) -> dict[str, int]:
        return {
            "hosts": len(self.devices),
            "sessions": len(self.sessions),
        }

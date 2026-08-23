"""Persistent allocation of 9-digit remote codes.

Once issued, an ID is never given to another device — including after the
holder goes offline or the signaling process restarts. The client may send
back the code it was already given; unknown codes are recorded on first
seen so already-deployed apps keep their numbers.
"""

from __future__ import annotations

import logging
import os
import sys
import threading
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

from remote.ids import generate_device_id, normalize_device_id

logger = logging.getLogger("remotedesk.server")


def resolve_id_store_path() -> Path | None:
    raw = (os.environ.get("DUSTX_ID_STORE") or "").strip()
    if raw.lower() in {"memory", "none", "off"}:
        return None
    if raw:
        return Path(raw)
    data = (os.environ.get("DUSTX_DATA_DIR") or "").strip()
    root = Path(data) if data else Path.home() / ".dustx"
    return root / "device_ids.txt"


class AllocatedIds:
    def __init__(self, path: Path | None = None) -> None:
        self.path = Path(path) if path else None
        self._lock = threading.Lock()
        self._issued: set[str] = set()
        if self.path:
            self._reload()

    def count(self) -> int:
        return len(self._issued)

    def issued(self) -> set[str]:
        return set(self._issued)

    def contains(self, device_id: str) -> bool:
        return normalize_device_id(device_id) in self._issued

    def reserve(self, device_id: str) -> str:
        """Record a client-held code so it is never reissued."""
        device_id = normalize_device_id(device_id)
        if len(device_id) != 9 or device_id == "000000000":
            raise ValueError("invalid device id")
        with self._lock:
            with self._file_lock():
                if self.path:
                    self._reload()
                if device_id not in self._issued:
                    self._issued.add(device_id)
                    self._append(device_id)
        return device_id

    def allocate(self, extra: set[str] | None = None) -> str:
        """Mint a new code that is not issued and not in ``extra``."""
        with self._lock:
            with self._file_lock():
                if self.path:
                    self._reload()
                taken = set(self._issued)
                if extra:
                    taken |= {normalize_device_id(x) for x in extra if x}
                device_id = generate_device_id(taken)
                self._issued.add(device_id)
                self._append(device_id)
                return device_id

    def attach_existing(self, device_ids: set[str]) -> None:
        for device_id in device_ids:
            n = normalize_device_id(device_id)
            if len(n) == 9 and n != "000000000":
                self.reserve(n)

    def _reload(self) -> None:
        if not self.path:
            return
        try:
            text = self.path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return
        for line in text.splitlines():
            n = normalize_device_id(line)
            if len(n) == 9 and n != "000000000":
                self._issued.add(n)

    def _append(self, device_id: str) -> None:
        if not self.path:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(device_id + "\n")
            fh.flush()
            os.fsync(fh.fileno())

    @contextmanager
    def _file_lock(self) -> Iterator[None]:
        if not self.path:
            yield
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        lock_path = self.path.with_name(self.path.name + ".lock")
        fh = lock_path.open("a+")
        try:
            if sys.platform != "win32":
                import fcntl

                fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
            yield
        finally:
            if sys.platform != "win32":
                import fcntl

                fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
            fh.close()


def attach_store(registry: object) -> None:
    """Point ``registry.ids`` at the on-disk allocator (no-op for memory mode)."""
    path = resolve_id_store_path()
    if path is None:
        return
    current = getattr(registry, "ids", None)
    if isinstance(current, AllocatedIds) and current.path is not None:
        return
    store = AllocatedIds(path)
    already: set[str] = set()
    if isinstance(current, AllocatedIds):
        already = current.issued()
    devices = getattr(registry, "devices", None)
    if isinstance(devices, dict):
        already |= set(devices)
    store.attach_existing(already)
    registry.ids = store  # type: ignore[attr-defined]
    logger.info("device id store %s (%d issued)", path, store.count())

"""Inbound file transfers from the viewer."""

from __future__ import annotations

from pathlib import Path


class FileInbox:
    def __init__(self, dest: Path | None = None) -> None:
        self.dest = dest or (Path.home() / "RemoteDeskDownloads")
        self.dest.mkdir(parents=True, exist_ok=True)
        self._open: dict[int, tuple[Path, int]] = {}

    def begin(self, transfer_id: int, name: str, size: int) -> Path:
        safe = Path(name).name or f"file-{transfer_id}"
        path = self.dest / safe
        path.write_bytes(b"")
        self._open[transfer_id] = (path, size)
        return path

    def write(self, transfer_id: int, offset: int, payload: bytes) -> None:
        item = self._open.get(transfer_id)
        if not item:
            return
        path, _size = item
        with path.open("r+b") as fh:
            fh.seek(offset)
            fh.write(payload)

    def finish(self, transfer_id: int) -> Path | None:
        item = self._open.pop(transfer_id, None)
        return item[0] if item else None

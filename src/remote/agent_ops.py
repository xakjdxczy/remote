"""Host-side application protocol for files and a remote shell.

Authenticated callers reach this only after the signaling server checks the
device password. Relative paths stay under the home directory. Absolute paths
are allowed when ``full=True`` (desktop file center / file transfer).
"""

from __future__ import annotations

import base64
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

MAX_FILE_BYTES = 1 << 20
MAX_CHUNK_BYTES = 384 << 10
MAX_EXEC_OUT = 256 << 10
MAX_LIST = 500
EXEC_TIMEOUT_SEC = 30
OPS = frozenset({"list", "read", "write", "exec", "mkdir", "rm", "volumes"})


def agent_root() -> Path:
    raw = (os.environ.get("DUSTX_AGENT_ROOT") or "").strip()
    if raw:
        return Path(raw).expanduser()
    return Path.home()


def resolve_agent_path(path: str, *, root: Path | None = None, full: bool = False) -> Path:
    root = (root or agent_root()).expanduser()
    try:
        root_n = root.resolve()
    except OSError as exc:
        raise ValueError(f"invalid agent root: {exc}") from exc
    raw = (path or "").strip()
    if not raw or raw == ".":
        candidate = root_n
    elif not full and raw in {"/", "\\"}:
        candidate = root_n
    else:
        given = Path(raw)
        candidate = given if given.is_absolute() else (root_n / given)
    candidate = Path(os.path.normpath(str(candidate)))
    try:
        resolved = candidate.resolve() if candidate.exists() else candidate.parent.resolve() / candidate.name
    except OSError as exc:
        raise ValueError(f"invalid path: {exc}") from exc
    if given_is_absolute(raw) and full:
        return resolved
    root_s = os.path.normcase(str(root_n))
    full_s = os.path.normcase(str(resolved))
    if full_s != root_s and not full_s.startswith(root_s + os.sep):
        raise ValueError("path outside home")
    return resolved


def given_is_absolute(raw: str) -> bool:
    text = (raw or "").strip()
    if not text:
        return False
    return Path(text).is_absolute()


def run_agent(
    op: str,
    *,
    path: str = "",
    content: str = "",
    command: str = "",
    cwd: str = "",
    offset: int = 0,
    length: int = 0,
    content_b64: str = "",
    full: bool = False,
) -> dict[str, Any]:
    kind = (op or "").strip().lower()
    if kind not in OPS:
        return {"ok": False, "error": "unknown op (list/read/write/exec/mkdir/rm/volumes)"}
    try:
        if kind == "volumes":
            return _volumes()
        if kind == "exec":
            return _exec(command, cwd, full=full)
        target = resolve_agent_path(path, full=full)
        if kind == "list":
            return _list(target)
        if kind == "read":
            return _read(target, offset=int(offset or 0), length=int(length or 0))
        if kind == "write":
            return _write(target, content, content_b64=content_b64, offset=int(offset or 0))
        if kind == "mkdir":
            return _mkdir(target)
        return _rm(target)
    except ValueError as exc:
        return {"ok": False, "error": str(exc)}
    except OSError as exc:
        return {"ok": False, "error": str(exc)}


def _list(target: Path) -> dict[str, Any]:
    if not target.exists():
        return {"ok": False, "error": "not found", "path": str(target)}
    if not target.is_dir():
        return {"ok": False, "error": "not a directory", "path": str(target)}
    entries: list[dict[str, Any]] = []
    try:
        names = sorted(target.iterdir(), key=lambda p: p.name.lower())
    except OSError as exc:
        return {"ok": False, "error": str(exc), "path": str(target)}
    for item in names:
        if len(entries) >= MAX_LIST:
            break
        try:
            is_dir = item.is_dir()
            size = 0 if is_dir else int(item.stat().st_size)
        except OSError:
            continue
        entries.append({"name": item.name, "dir": is_dir, "size": size})
    return {"ok": True, "op": "list", "path": str(target), "entries": entries}


def _read(target: Path, *, offset: int = 0, length: int = 0) -> dict[str, Any]:
    if not target.exists() or not target.is_file():
        return {"ok": False, "error": "not a file", "path": str(target)}
    size = int(target.stat().st_size)
    if length > 0 or offset > 0:
        if offset < 0 or offset > size:
            return {"ok": False, "error": "bad offset", "path": str(target), "size": size}
        take = length if length > 0 else min(MAX_CHUNK_BYTES, size - offset)
        if take > MAX_CHUNK_BYTES:
            return {"ok": False, "error": "chunk too large"}
        with target.open("rb") as fh:
            fh.seek(offset)
            data = fh.read(take)
        return {
            "ok": True,
            "op": "read",
            "path": str(target),
            "size": size,
            "offset": offset,
            "bytes": len(data),
            "content_b64": base64.b64encode(data).decode("ascii"),
        }
    if size > MAX_FILE_BYTES:
        return {"ok": False, "error": f"file too large ({size} bytes)", "path": str(target), "size": size}
    data = target.read_bytes()
    return {
        "ok": True,
        "op": "read",
        "path": str(target),
        "size": size,
        "content": data.decode("utf-8", "replace"),
    }


def _write(target: Path, content: str, *, content_b64: str = "", offset: int = 0) -> dict[str, Any]:
    if content_b64:
        try:
            data = base64.b64decode(content_b64, validate=False)
        except Exception:
            return {"ok": False, "error": "invalid content_b64"}
        if len(data) > MAX_CHUNK_BYTES:
            return {"ok": False, "error": "content too large"}
        if offset < 0:
            return {"ok": False, "error": "bad offset"}
        target.parent.mkdir(parents=True, exist_ok=True)
        mode = "r+b" if target.exists() and offset > 0 else "wb"
        if mode == "wb" and offset > 0:
            return {"ok": False, "error": "file not found"}
        with target.open(mode) as fh:
            if offset > 0:
                fh.seek(offset)
            fh.write(data)
        return {"ok": True, "op": "write", "path": str(target), "bytes": len(data), "offset": offset}
    data = (content or "").encode("utf-8")
    if len(data) > MAX_FILE_BYTES:
        return {"ok": False, "error": "content too large"}
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {"ok": True, "op": "write", "path": str(target), "bytes": len(data)}


def _mkdir(target: Path) -> dict[str, Any]:
    target.mkdir(parents=True, exist_ok=True)
    return {"ok": True, "op": "mkdir", "path": str(target)}


def _is_volume_root(target: Path) -> bool:
    try:
        resolved = target.resolve()
    except OSError:
        resolved = target
    if resolved.parent == resolved:
        return True
    text = os.path.normpath(str(resolved))
    if len(text) == 3 and text[1] == ":" and text[2] in "\\/":
        return True
    return False


def _rm(target: Path) -> dict[str, Any]:
    if not target.exists():
        return {"ok": False, "error": "not found", "path": str(target)}
    if _is_volume_root(target):
        return {"ok": False, "error": "refusing to delete a volume root", "path": str(target)}
    if target.is_dir() and not target.is_symlink():
        shutil.rmtree(target)
    else:
        target.unlink()
    return {"ok": True, "op": "rm", "path": str(target)}


def _volumes() -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    if os.name == "nt":
        import ctypes

        mask = ctypes.windll.kernel32.GetLogicalDrives()
        for i in range(26):
            if not mask & (1 << i):
                continue
            letter = chr(65 + i)
            path = f"{letter}:\\"
            entries.append({"name": f"{letter}:", "path": path, "dir": True})
    else:
        entries.append({"name": "/", "path": "/", "dir": True})
        for base in (Path("/Volumes"), Path("/media"), Path("/mnt")):
            if not base.is_dir():
                continue
            try:
                kids = sorted(base.iterdir(), key=lambda p: p.name.lower())
            except OSError:
                continue
            for item in kids:
                try:
                    if item.is_dir():
                        entries.append({"name": item.name, "path": str(item), "dir": True})
                except OSError:
                    continue
    return {"ok": True, "op": "volumes", "entries": entries}


def _exec(command: str, cwd: str, *, full: bool = False) -> dict[str, Any]:
    cmd = (command or "").strip()
    if not cmd:
        return {"ok": False, "error": "command required"}
    work = resolve_agent_path(cwd, full=full) if cwd.strip() else agent_root().resolve()
    if not work.is_dir():
        return {"ok": False, "error": "cwd is not a directory", "path": str(work)}
    try:
        proc = subprocess.run(
            cmd,
            shell=True,
            cwd=str(work),
            capture_output=True,
            timeout=EXEC_TIMEOUT_SEC,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "op": "exec", "error": "timeout", "exit": -1, "stdout": "", "stderr": ""}
    stdout = proc.stdout[:MAX_EXEC_OUT].decode("utf-8", "replace")
    stderr = proc.stderr[:MAX_EXEC_OUT].decode("utf-8", "replace")
    return {
        "ok": True,
        "op": "exec",
        "exit": int(proc.returncode),
        "stdout": stdout,
        "stderr": stderr,
        "cwd": str(work),
    }

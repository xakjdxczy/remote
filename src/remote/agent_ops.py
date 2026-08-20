"""Host-side application protocol: list / read / write / exec under the home dir.

Used by the Python host and mirrored by the C++ desktop app. Authenticated
callers reach this only after the signaling server checks the device password.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Any

MAX_FILE_BYTES = 1 << 20
MAX_EXEC_OUT = 256 << 10
MAX_LIST = 500
EXEC_TIMEOUT_SEC = 30
OPS = frozenset({"list", "read", "write", "exec"})


def agent_root() -> Path:
    raw = (os.environ.get("DUSTX_AGENT_ROOT") or "").strip()
    if raw:
        return Path(raw).expanduser()
    return Path.home()


def resolve_agent_path(path: str, *, root: Path | None = None) -> Path:
    root = (root or agent_root()).expanduser()
    try:
        root_n = root.resolve()
    except OSError as exc:
        raise ValueError(f"invalid agent root: {exc}") from exc
    raw = (path or "").strip()
    if not raw or raw in {".", "/", "\\"}:
        candidate = root_n
    else:
        given = Path(raw)
        candidate = given if given.is_absolute() else (root_n / given)
    candidate = Path(os.path.normpath(str(candidate)))
    try:
        full = candidate.resolve() if candidate.exists() else candidate.parent.resolve() / candidate.name
    except OSError as exc:
        raise ValueError(f"invalid path: {exc}") from exc
    root_s = os.path.normcase(str(root_n))
    full_s = os.path.normcase(str(full))
    if full_s != root_s and not full_s.startswith(root_s + os.sep):
        raise ValueError("path outside home")
    return full


def run_agent(
    op: str,
    *,
    path: str = "",
    content: str = "",
    command: str = "",
    cwd: str = "",
) -> dict[str, Any]:
    kind = (op or "").strip().lower()
    if kind not in OPS:
        return {"ok": False, "error": "unknown op (list/read/write/exec)"}
    try:
        if kind == "exec":
            return _exec(command, cwd)
        target = resolve_agent_path(path)
        if kind == "list":
            return _list(target)
        if kind == "read":
            return _read(target)
        return _write(target, content)
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


def _read(target: Path) -> dict[str, Any]:
    if not target.exists() or not target.is_file():
        return {"ok": False, "error": "not a file", "path": str(target)}
    size = target.stat().st_size
    if size > MAX_FILE_BYTES:
        return {"ok": False, "error": f"file too large ({size} bytes)", "path": str(target)}
    data = target.read_bytes()
    return {"ok": True, "op": "read", "path": str(target), "content": data.decode("utf-8", "replace")}


def _write(target: Path, content: str) -> dict[str, Any]:
    data = (content or "").encode("utf-8")
    if len(data) > MAX_FILE_BYTES:
        return {"ok": False, "error": "content too large"}
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {"ok": True, "op": "write", "path": str(target), "bytes": len(data)}


def _exec(command: str, cwd: str) -> dict[str, Any]:
    cmd = (command or "").strip()
    if not cmd:
        return {"ok": False, "error": "command required"}
    work = resolve_agent_path(cwd) if cwd.strip() else agent_root().resolve()
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
    }

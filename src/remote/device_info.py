"""User-facing DustX version and sanitized machine inventory."""

from __future__ import annotations

import json
import os
import platform
import shutil
import socket
import time
from typing import Any

APP_VERSION = "2026.8.21.6"
MAX_STR = 160
MAX_DISKS = 16
MAX_INFO_BYTES = 16 * 1024

_STR_KEYS = (
    "version",
    "hostname",
    "model",
    "cpu",
    "cpu_arch",
    "board",
    "gpu",
    "os",
    "os_build",
)
_INT_KEYS = ("cpu_cores", "ram_bytes", "ram_used_bytes", "uptime_sec")


def clip(value: Any, n: int = MAX_STR) -> str:
    text = " ".join(str(value or "").split())
    return text[:n]


def sanitize_info(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        return {}
    out: dict[str, Any] = {}
    for key in _STR_KEYS:
        if key in raw and raw[key] not in (None, ""):
            out[key] = clip(raw[key])
    for key in _INT_KEYS:
        if key not in raw or raw[key] in (None, ""):
            continue
        try:
            val = int(raw[key])
        except (TypeError, ValueError):
            continue
        if val < 0:
            continue
        out[key] = val
    disks = raw.get("disks")
    if isinstance(disks, list):
        cleaned: list[dict[str, Any]] = []
        for item in disks[:MAX_DISKS]:
            if not isinstance(item, dict):
                continue
            try:
                total = int(item.get("total") or 0)
                used = int(item.get("used") or 0)
            except (TypeError, ValueError):
                continue
            if total <= 0:
                continue
            cleaned.append({
                "name": clip(item.get("name") or item.get("path") or "disk", 80),
                "path": clip(item.get("path") or "", 80),
                "total": total,
                "used": max(0, min(used, total)),
                "kind": clip(item.get("kind") or "", 24),
            })
        if cleaned:
            out["disks"] = cleaned
    if not out.get("version"):
        out["version"] = APP_VERSION
    encoded = json.dumps(out, ensure_ascii=False).encode("utf-8")
    if len(encoded) > MAX_INFO_BYTES:
        out.pop("disks", None)
    return out


def collect_info() -> dict[str, Any]:
    info: dict[str, Any] = {
        "version": APP_VERSION,
        "hostname": clip(platform.node() or socket.gethostname()),
        "cpu_arch": clip(platform.machine()),
        "os": clip(f"{platform.system()} {platform.release()}".strip()),
        "cpu_cores": os.cpu_count() or 0,
    }
    try:
        if hasattr(time, "CLOCK_BOOTTIME"):
            info["uptime_sec"] = int(time.clock_gettime(time.CLOCK_BOOTTIME))
    except OSError:
        pass
    if os.name == "nt":
        info.update(_windows())
    elif platform.system() == "Darwin":
        info.update(_darwin())
    else:
        info.update(_linux())
    return sanitize_info(info)


def _read(path: str) -> str:
    try:
        return PathRead(path)
    except OSError:
        return ""


def PathRead(path: str) -> str:
    from pathlib import Path

    return Path(path).read_text(encoding="utf-8", errors="replace").strip()


def _linux() -> dict[str, Any]:
    out: dict[str, Any] = {}
    cpu = ""
    for line in _read("/proc/cpuinfo").splitlines():
        if line.lower().startswith("model name") or line.lower().startswith("hardware"):
            cpu = line.split(":", 1)[-1].strip()
            break
    if cpu:
        out["cpu"] = cpu
    mem_total = mem_avail = 0
    for line in _read("/proc/meminfo").splitlines():
        if line.startswith("MemTotal:"):
            mem_total = int(line.split()[1]) * 1024
        elif line.startswith("MemAvailable:"):
            mem_avail = int(line.split()[1]) * 1024
    if mem_total:
        out["ram_bytes"] = mem_total
        out["ram_used_bytes"] = max(0, mem_total - mem_avail) if mem_avail else 0
    board = _read("/sys/class/dmi/id/board_name") or _read("/sys/class/dmi/id/board_serial")
    model = _read("/sys/class/dmi/id/product_name")
    if board:
        out["board"] = board
    if model:
        out["model"] = model
    gpu = ""
    try:
        from pathlib import Path

        for card in sorted(Path("/sys/class/drm").glob("card*-*/device/vendor")):
            parent = card.parent
            name = (parent / "subsystem_device").read_text(encoding="utf-8", errors="replace") if False else ""
            uevent = _read(str(parent / "uevent"))
            for line in uevent.splitlines():
                if line.startswith("DRIVER=") or line.startswith("PCI_ID="):
                    gpu = line.split("=", 1)[-1]
            if gpu:
                break
    except OSError:
        pass
    if gpu:
        out["gpu"] = gpu
    disks = []
    try:
        usage = shutil.disk_usage("/")
        disks.append({"name": "/", "path": "/", "total": usage.total, "used": usage.used, "kind": "fixed"})
    except OSError:
        pass
    if disks:
        out["disks"] = disks
    try:
        with open("/proc/uptime", encoding="utf-8") as fh:
            out["uptime_sec"] = int(float(fh.read().split()[0]))
    except (OSError, ValueError, IndexError):
        pass
    return out


def _darwin() -> dict[str, Any]:
    out: dict[str, Any] = {}
    out["os"] = f"macOS {platform.mac_ver()[0]}".strip()
    out["model"] = _sysctl("hw.model")
    out["cpu"] = _sysctl("machdep.cpu.brand_string") or _sysctl("hw.ncpu")
    try:
        out["ram_bytes"] = int(_sysctl("hw.memsize") or "0")
    except ValueError:
        pass
    return out


def _sysctl(name: str) -> str:
    import subprocess

    try:
        proc = subprocess.run(["sysctl", "-n", name], capture_output=True, text=True, timeout=2)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return (proc.stdout or "").strip()


def _windows() -> dict[str, Any]:
    out: dict[str, Any] = {"os": clip(f"{platform.system()} {platform.release()}")}
    try:
        import ctypes
        from ctypes import wintypes

        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [
                ("dwLength", wintypes.DWORD),
                ("dwMemoryLoad", wintypes.DWORD),
                ("ullTotalPhys", ctypes.c_uint64),
                ("ullAvailPhys", ctypes.c_uint64),
                ("ullTotalPageFile", ctypes.c_uint64),
                ("ullAvailPageFile", ctypes.c_uint64),
                ("ullTotalVirtual", ctypes.c_uint64),
                ("ullAvailVirtual", ctypes.c_uint64),
                ("ullAvailExtendedVirtual", ctypes.c_uint64),
            ]

        mem = MEMORYSTATUSEX()
        mem.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(mem)):
            out["ram_bytes"] = int(mem.ullTotalPhys)
            out["ram_used_bytes"] = int(mem.ullTotalPhys - mem.ullAvailPhys)
    except Exception:
        pass
    return out

"""Normalize motherboard / NIC / system UUID into one hardware identity."""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from typing import Any

_PLACEHOLDERS = frozenset({
    "",
    "none",
    "null",
    "n/a",
    "na",
    "unknown",
    "default string",
    "defaultstring",
    "to be filled by o.e.m.",
    "tobefilledbyo.e.m.",
    "to be filled",
    "system serial number",
    "system product name",
    "0",
    "000000000000",
    "ffffffffffff",
    "00000000-0000-0000-0000-000000000000",
    "ffffffff-ffff-ffff-ffff-ffffffffffff",
})


def _clean(value: str) -> str:
    return re.sub(r"\s+", " ", str(value or "")).strip()


def normalize_board(value: str) -> str:
    text = _clean(value).upper()
    compact = re.sub(r"[\s\-_.]+", "", text)
    if text.lower() in _PLACEHOLDERS or compact.lower() in _PLACEHOLDERS:
        return ""
    return text


def normalize_nic(value: str) -> str:
    hexes = re.sub(r"[^0-9a-fA-F]", "", str(value or "")).lower()
    if len(hexes) != 12:
        return ""
    if hexes in {"000000000000", "ffffffffffff"}:
        return ""
    return hexes


def normalize_uuid(value: str) -> str:
    hexes = re.sub(r"[^0-9a-fA-F]", "", str(value or "")).lower()
    if len(hexes) != 32:
        return ""
    if hexes in {"0" * 32, "f" * 32}:
        return ""
    return hexes


@dataclass(frozen=True)
class HardwareFingerprint:
    board: str
    nic: str
    sys_uuid: str

    @property
    def complete(self) -> bool:
        return bool(self.board and self.nic and self.sys_uuid)

    @property
    def key(self) -> str:
        raw = f"{self.board}|{self.nic}|{self.sys_uuid}".encode("utf-8")
        return hashlib.sha256(raw).hexdigest()

    def as_row(self) -> dict[str, str]:
        return {"board": self.board, "nic": self.nic, "sys_uuid": self.sys_uuid}


def parse_fingerprint(raw: Any) -> HardwareFingerprint | None:
    if not isinstance(raw, dict):
        return None
    fp = HardwareFingerprint(
        board=normalize_board(str(raw.get("board") or raw.get("motherboard") or "")),
        nic=normalize_nic(str(raw.get("nic") or raw.get("mac") or "")),
        sys_uuid=normalize_uuid(str(raw.get("uuid") or raw.get("sys_uuid") or "")),
    )
    return fp if fp.complete else None


def collect_fingerprint() -> dict[str, str] | None:
    """Best-effort local read for the Python host agent. Desktop uses C++."""
    import platform

    system = platform.system()
    try:
        if system == "Darwin":
            fp = _collect_darwin()
        elif system == "Windows":
            fp = _collect_windows()
        else:
            fp = _collect_linux()
    except Exception:
        return None
    parsed = parse_fingerprint(fp)
    return parsed.as_row() | {"uuid": parsed.sys_uuid} if parsed else None


def _ioreg_field(text: str, key: str) -> str:
    match = re.search(rf'"{re.escape(key)}"\s*=\s*"([^"]+)"', text)
    return match.group(1) if match else ""


def _mac_from_ifconfig(iface: str) -> str:
    import subprocess

    out = subprocess.check_output(["ifconfig", iface], text=True, timeout=3)
    match = re.search(r"\bether\s+([0-9a-fA-F:]{17})", out)
    return match.group(1) if match else ""


def _collect_darwin() -> dict[str, str]:
    import subprocess

    ioreg = subprocess.check_output(
        ["ioreg", "-rd1", "-c", "IOPlatformExpertDevice"],
        text=True,
        timeout=5,
    )
    nic = ""
    for iface in ("en0", "en1", "en2"):
        try:
            nic = _mac_from_ifconfig(iface)
        except Exception:
            nic = ""
        if normalize_nic(nic):
            break
    return {
        "board": _ioreg_field(ioreg, "IOPlatformSerialNumber"),
        "nic": nic,
        "uuid": _ioreg_field(ioreg, "IOPlatformUUID"),
    }


def _collect_windows() -> dict[str, str]:
    import subprocess

    script = (
        "$b=(Get-CimInstance Win32_BaseBoard);"
        "$s=(Get-CimInstance Win32_ComputerSystemProduct);"
        "$n=(Get-CimInstance Win32_NetworkAdapter -Filter \"NetEnabled=TRUE\" |"
        " Where-Object { $_.MACAddress -and $_.PNPDeviceID -notmatch 'ROOT\\\\' -and $_.AdapterType -notmatch 'Tunnel' } |"
        " Select-Object -First 1);"
        "Write-Output ($b.SerialNumber);"
        "Write-Output ($n.MACAddress);"
        "Write-Output ($s.UUID);"
    )
    out = subprocess.check_output(
        ["powershell", "-NoProfile", "-Command", script],
        text=True,
        timeout=15,
    )
    lines = [ln.strip() for ln in out.splitlines() if ln.strip()]
    board = lines[0] if len(lines) > 0 else ""
    nic = lines[1] if len(lines) > 1 else ""
    uuid = lines[2] if len(lines) > 2 else ""
    return {"board": board, "nic": nic, "uuid": uuid}


def _collect_linux() -> dict[str, str]:
    from pathlib import Path

    def read(path: str) -> str:
        try:
            return Path(path).read_text(encoding="utf-8").strip()
        except Exception:
            return ""

    board = read("/sys/class/dmi/id/board_serial") or read("/sys/class/dmi/id/product_serial")
    uuid = read("/sys/class/dmi/id/product_uuid")
    nic = ""
    net = Path("/sys/class/net")
    if net.is_dir():
        for iface in sorted(p.name for p in net.iterdir()):
            if iface.startswith(("lo", "veth", "docker", "br-", "virbr", "tun", "tap", "wg")):
                continue
            nic = read(str(net / iface / "address"))
            if normalize_nic(nic):
                break
    return {"board": board, "nic": nic, "uuid": uuid}

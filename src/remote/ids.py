"""Device ID and temporary password generation."""

from __future__ import annotations

import secrets
import string

ID_DIGITS = 9
PASSWORD_CHARS = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789"


def generate_device_id(existing: set[str] | None = None) -> str:
    """Return a 9-digit ID that is not all zeros and not already taken."""
    existing = existing or set()
    for _ in range(64):
        n = secrets.randbelow(10**ID_DIGITS - 1) + 1
        device_id = f"{n:0{ID_DIGITS}d}"
        if device_id not in existing:
            return device_id
    raise RuntimeError("unable to allocate a unique device id")


def format_device_id(device_id: str) -> str:
    digits = "".join(ch for ch in device_id if ch.isdigit())
    if len(digits) != ID_DIGITS:
        return device_id
    return f"{digits[0:3]} {digits[3:6]} {digits[6:9]}"


def normalize_device_id(device_id: str) -> str:
    return "".join(ch for ch in device_id if ch.isdigit())


def generate_temp_password(length: int = 8) -> str:
    return "".join(secrets.choice(PASSWORD_CHARS) for _ in range(length))


def constant_time_equals(left: str, right: str) -> bool:
    return secrets.compare_digest(left.encode("utf-8"), right.encode("utf-8"))

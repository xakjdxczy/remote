"""Compare dotted DustX versions like 2026.8.21.4."""

from __future__ import annotations

import re


def parse_version(raw: str | None) -> tuple[int, ...]:
    parts = [int(p) for p in re.findall(r"\d+", str(raw or ""))]
    return tuple(parts) if parts else (0,)


def version_less(current: str | None, other: str | None) -> bool:
    return parse_version(current) < parse_version(other)


def version_equal(current: str | None, other: str | None) -> bool:
    return parse_version(current) == parse_version(other)

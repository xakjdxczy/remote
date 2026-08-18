"""Frozen desktop entry. Users open 尘埃X.app / 尘埃X.exe — not Python."""

from __future__ import annotations

import sys
from pathlib import Path

if not getattr(sys, "frozen", False):
    src = Path(__file__).resolve().parent.parent / "src"
    sys.path.insert(0, str(src))

from remote.desktop_app import run_app


if __name__ == "__main__":
    run_app()

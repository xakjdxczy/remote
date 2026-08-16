"""Frame source selection: desktop / android / virtual backends.

A *frame source* both produces JPEG frames and consumes viewer input, so the
host agent stays backend-agnostic. Backends:

* ``desktop`` – real screen on Windows/macOS/Linux (:mod:`remote.host.desktop`).
* ``android`` – an ADB-connected Android device (:mod:`remote.host.android`).
* ``virtual`` – a drawn demo desktop, used headless or for demos.
"""

from __future__ import annotations

import logging
import os
import sys
import time
from typing import Protocol

from remote.host.virtual_desktop import VirtualDesktop

logger = logging.getLogger("remotedesk.host.capture")

BACKENDS = ("auto", "desktop", "android", "virtual")


class FrameSource(Protocol):
    width: int
    height: int

    def grab_jpeg(self, quality: int = 70) -> bytes: ...
    def grab_image(self): ...
    def handle_mouse(self, event: str, x: int, y: int, button: str = "left") -> None: ...
    def handle_key(self, event: str, key: str) -> None: ...
    def backend_name(self) -> str: ...


def has_display() -> bool:
    if sys.platform == "darwin":
        return True
    if sys.platform == "win32":
        return True
    return bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"))


class VirtualSource:
    def __init__(self) -> None:
        self.desktop = VirtualDesktop()
        self.width = self.desktop.width
        self.height = self.desktop.height

    def grab_jpeg(self, quality: int = 70) -> bytes:
        return self.desktop.render_jpeg(quality=quality)

    def grab_image(self):
        return self.desktop.render_image()

    def handle_mouse(self, event: str, x: int, y: int, button: str = "left") -> None:
        self.desktop.handle_mouse(event, x, y, button)

    def handle_key(self, event: str, key: str) -> None:
        self.desktop.handle_key(event, key)

    def backend_name(self) -> str:
        return "virtual"


def open_frame_source(
    prefer_virtual: bool = False,
    *,
    backend: str = "auto",
    adb_serial: str | None = None,
    max_width: int = 1280,
) -> FrameSource:
    """Create a frame source for the requested backend.

    ``prefer_virtual`` is kept for backwards compatibility and is equivalent to
    ``backend="virtual"``.
    """
    if prefer_virtual:
        backend = "virtual"
    if backend not in BACKENDS:
        raise ValueError(f"unknown backend {backend!r}; choose from {BACKENDS}")

    if backend == "virtual":
        return VirtualSource()

    if backend == "android":
        from remote.host.android import AndroidAdbSource

        return AndroidAdbSource(serial=adb_serial, max_width=max_width)

    if backend == "desktop":
        return _open_desktop(max_width) or VirtualSource()

    # auto
    if has_display():
        source = _open_desktop(max_width)
        if source is not None:
            return source
        logger.info("无法打开真实屏幕，回退到演示桌面")
    return VirtualSource()


def _open_desktop(max_width: int) -> "FrameSource | None":
    from remote.host.desktop import DesktopSource

    try:
        return DesktopSource(max_width=max_width)
    except Exception as exc:
        logger.warning("desktop backend unavailable: %s", exc)
        return None


def now_ms() -> int:
    return int(time.time() * 1000)


# Backwards-compatible alias for the former class name.
def MssSource() -> FrameSource:  # noqa: N802 - kept for compatibility
    from remote.host.desktop import DesktopSource

    return DesktopSource()

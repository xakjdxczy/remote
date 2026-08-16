"""Real-screen capture + input injection for Windows, macOS and Linux.

Uses ``mss`` for screen grabbing and ``pynput`` for mouse/keyboard injection.
Both libraries are cross-platform, so a single backend covers desktop operating
systems. Platform-specific concerns handled here:

* macOS Retina displays report *logical* points in ``mss`` monitor geometry but
  grab *physical* pixels, so viewer coordinates are mapped back to logical
  points (and offset by the monitor origin) before being handed to ``pynput``.
* macOS requires the user to grant *Screen Recording* and *Accessibility*
  permissions; :func:`macos_permission_hint` returns guidance when capture or
  input appears to be blocked.

The class takes optional injected collaborators (``grabber``/``mouse``/
``keyboard``) so the coordinate and key mapping can be unit tested without a
real display.
"""

from __future__ import annotations

import io
import logging
import sys
from typing import Any, Callable

logger = logging.getLogger("remotedesk.host.desktop")

# Viewer key name -> pynput special key attribute name.
_SPECIAL_KEYS = {
    "Enter": "enter",
    "Return": "enter",
    "Backspace": "backspace",
    "Tab": "tab",
    "Escape": "esc",
    "Esc": "esc",
    "Delete": "delete",
    "Home": "home",
    "End": "end",
    "PageUp": "page_up",
    "PageDown": "page_down",
    "ArrowLeft": "left",
    "ArrowRight": "right",
    "ArrowUp": "up",
    "ArrowDown": "down",
    "Shift": "shift",
    "Control": "ctrl",
    "Alt": "alt",
    "Meta": "cmd",
    "CapsLock": "caps_lock",
    " ": "space",
    "F1": "f1", "F2": "f2", "F3": "f3", "F4": "f4", "F5": "f5", "F6": "f6",
    "F7": "f7", "F8": "f8", "F9": "f9", "F10": "f10", "F11": "f11", "F12": "f12",
}


def macos_permission_hint() -> str:
    return (
        "macOS 需要授权：系统设置 → 隐私与安全性 → 屏幕录制 / 辅助功能，"
        "勾选运行本程序的终端或应用，然后重启被控端。"
    )


class DesktopSource:
    """Capture the primary monitor and inject mouse/keyboard events."""

    def __init__(
        self,
        *,
        max_width: int = 1280,
        grabber: Any | None = None,
        monitor: dict | None = None,
        mouse: Any | None = None,
        keyboard: Any | None = None,
    ) -> None:
        from PIL import Image  # local import keeps import errors actionable

        self._Image = Image
        self._max_width = max_width
        self._platform = sys.platform

        if grabber is None:
            import mss  # type: ignore

            self._mss = mss.mss()
            monitors = self._mss.monitors
            monitor = monitor or (monitors[1] if len(monitors) > 1 else monitors[0])
            self._grab = self._mss.grab
        else:
            self._mss = None
            self._grab = grabber
            monitor = monitor or {"left": 0, "top": 0, "width": max_width, "height": int(max_width * 9 / 16)}

        self._monitor = monitor
        # Logical geometry of the captured monitor (points on macOS Retina).
        self._logical_w = int(monitor["width"])
        self._logical_h = int(monitor["height"])
        self._left = int(monitor.get("left", 0))
        self._top = int(monitor.get("top", 0))
        # Reported (viewer-facing) size; updated on every grab after any resize.
        self.width = self._logical_w
        self.height = self._logical_h

        self._mouse = mouse if mouse is not None else self._make_mouse()
        self._keyboard = keyboard if keyboard is not None else self._make_keyboard()
        self._input_ok = self._mouse is not None and self._keyboard is not None
        self._warned_input = False

    # -- construction helpers -------------------------------------------------
    def _make_mouse(self):
        try:
            from pynput.mouse import Controller as MouseCtl

            return MouseCtl()
        except Exception as exc:  # pragma: no cover - depends on platform libs
            logger.warning("mouse control unavailable: %s", exc)
            return None

    def _make_keyboard(self):
        try:
            from pynput.keyboard import Controller as KeyCtl

            return KeyCtl()
        except Exception as exc:  # pragma: no cover - depends on platform libs
            logger.warning("keyboard control unavailable: %s", exc)
            return None

    # -- capture --------------------------------------------------------------
    def grab_jpeg(self, quality: int = 70) -> bytes:
        raw = self._grab(self._monitor)
        img = self._Image.frombytes("RGB", raw.size, raw.bgra, "raw", "BGRX")
        if img.width > self._max_width:
            ratio = self._max_width / img.width
            img = img.resize((self._max_width, max(1, int(img.height * ratio))))
        self.width, self.height = img.size
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=quality, optimize=True)
        return buf.getvalue()

    # -- coordinate mapping ---------------------------------------------------
    def _to_logical(self, x: int, y: int) -> tuple[int, int]:
        """Map a viewer-space point to logical screen coordinates."""
        rw = self.width or self._logical_w
        rh = self.height or self._logical_h
        lx = self._left + x * self._logical_w / rw
        ly = self._top + y * self._logical_h / rh
        return int(round(lx)), int(round(ly))

    # -- input ----------------------------------------------------------------
    def handle_mouse(self, event: str, x: int, y: int, button: str = "left") -> None:
        if not self._mouse:
            self._warn_input()
            return
        lx, ly = self._to_logical(x, y)
        self._mouse.position = (lx, ly)
        btn = self._resolve_button(button)
        try:
            if event == "down":
                self._mouse.press(btn)
            elif event == "up":
                self._mouse.release(btn)
            elif event == "scroll":
                self._mouse.scroll(0, 1 if button == "up" else -1)
        except Exception as exc:  # pragma: no cover - platform dependent
            logger.debug("mouse event failed: %s", exc)

    def handle_key(self, event: str, key: str) -> None:
        if not self._keyboard:
            self._warn_input()
            return
        target = self._resolve_key(key)
        if target is None:
            return
        try:
            if event == "down":
                self._keyboard.press(target)
            else:
                self._keyboard.release(target)
        except Exception as exc:  # pragma: no cover - platform dependent
            logger.debug("key event failed: %s", exc)

    def _resolve_button(self, button: str):
        try:
            from pynput.mouse import Button
        except Exception:  # headless / no platform backend
            return button
        mapping = {"left": Button.left, "right": Button.right, "middle": Button.middle}
        return mapping.get(button, Button.left)

    def _resolve_key(self, key: str):
        name = _SPECIAL_KEYS.get(key)
        if name:
            try:
                from pynput.keyboard import Key
            except Exception:  # headless / no platform backend
                return name
            return getattr(Key, name, None)
        if len(key) == 1:
            return key
        return None

    def _warn_input(self) -> None:
        if self._warned_input:
            return
        self._warned_input = True
        if self._platform == "darwin":
            logger.warning("输入注入不可用。%s", macos_permission_hint())
        else:
            logger.warning("输入注入不可用（缺少 pynput 或无权限）。")

    def backend_name(self) -> str:
        return {"darwin": "macos", "win32": "windows"}.get(self._platform, "desktop")

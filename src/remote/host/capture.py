"""Screen capture with real display fallback to the virtual desktop."""

from __future__ import annotations

import os
import sys
import time
from typing import Protocol

from remote.host.virtual_desktop import VirtualDesktop


class FrameSource(Protocol):
    width: int
    height: int

    def grab_jpeg(self, quality: int = 70) -> bytes: ...
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

    def handle_mouse(self, event: str, x: int, y: int, button: str = "left") -> None:
        self.desktop.handle_mouse(event, x, y, button)

    def handle_key(self, event: str, key: str) -> None:
        self.desktop.handle_key(event, key)

    def backend_name(self) -> str:
        return "virtual"


class MssSource:
    def __init__(self) -> None:
        import mss  # type: ignore
        from PIL import Image
        import io

        self._mss = mss.mss()
        self._Image = Image
        self._io = io
        mon = self._mss.monitors[1] if len(self._mss.monitors) > 1 else self._mss.monitors[0]
        self.width = int(mon["width"])
        self.height = int(mon["height"])
        self._monitor = mon
        self._controller = None
        self._keyboard = None
        try:
            from pynput.mouse import Controller as MouseCtl
            from pynput.keyboard import Controller as KeyCtl

            self._controller = MouseCtl()
            self._keyboard = KeyCtl()
        except Exception:
            pass

    def grab_jpeg(self, quality: int = 70) -> bytes:
        raw = self._mss.grab(self._monitor)
        img = self._Image.frombytes("RGB", raw.size, raw.bgra, "raw", "BGRX")
        max_w = 1280
        if img.width > max_w:
            ratio = max_w / img.width
            img = img.resize((max_w, int(img.height * ratio)))
            self.width, self.height = img.size
        buf = self._io.BytesIO()
        img.save(buf, format="JPEG", quality=quality, optimize=True)
        return buf.getvalue()

    def handle_mouse(self, event: str, x: int, y: int, button: str = "left") -> None:
        if not self._controller:
            return
        from pynput.mouse import Button

        self._controller.position = (int(x), int(y))
        mapping = {"left": Button.left, "right": Button.right, "middle": Button.middle}
        btn = mapping.get(button, Button.left)
        if event == "down":
            self._controller.press(btn)
        elif event == "up":
            self._controller.release(btn)
        elif event == "scroll":
            self._controller.scroll(0, 1 if button == "up" else -1)

    def handle_key(self, event: str, key: str) -> None:
        if not self._keyboard:
            return
        from pynput.keyboard import Key

        special = {
            "Enter": Key.enter,
            "Backspace": Key.backspace,
            "Tab": Key.tab,
            "Escape": Key.esc,
            "ArrowLeft": Key.left,
            "ArrowRight": Key.right,
            "ArrowUp": Key.up,
            "ArrowDown": Key.down,
            "Shift": Key.shift,
            "Control": Key.ctrl,
            "Alt": Key.alt,
            "Meta": Key.cmd,
            " ": Key.space,
        }
        target = special.get(key, key if len(key) == 1 else None)
        if target is None:
            return
        try:
            if event == "down":
                self._keyboard.press(target)
            else:
                self._keyboard.release(target)
        except Exception:
            pass

    def backend_name(self) -> str:
        return "mss"


def open_frame_source(prefer_virtual: bool = False) -> FrameSource:
    if not prefer_virtual and has_display():
        try:
            return MssSource()
        except Exception:
            pass
    return VirtualSource()


def now_ms() -> int:
    return int(time.time() * 1000)

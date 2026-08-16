"""Android controlled-side backend via the ADB bridge.

The RemoteDesk host runs on a computer (Linux/macOS/Windows) with an Android
device attached over USB or ``adb connect`` (Wi-Fi). Frames come from
``adb exec-out screencap -p`` and input is injected with ``adb shell input``.
This keeps the whole host in Python and reuses the existing WebRTC pipeline, so
an Android phone/tablet becomes remotely controllable without a separate native
app.

A fully standalone on-device Android app (MediaProjection + AccessibilityService
+ native WebRTC) is a larger, separate effort and is intentionally out of scope
here; the ADB bridge is the pragmatic, testable path that fits this codebase.

The ``runner`` collaborator is injectable so command construction and
coordinate/key mapping can be unit tested without a real device.
"""

from __future__ import annotations

import io
import logging
import re
import subprocess
from typing import Callable

logger = logging.getLogger("remotedesk.host.android")

# Viewer key name -> Android key event code (KEYCODE_*).
_KEYEVENTS = {
    "Enter": 66,
    "Return": 66,
    "Backspace": 67,
    "Tab": 61,
    "Escape": 111,
    "Delete": 112,
    "ArrowLeft": 21,
    "ArrowRight": 22,
    "ArrowUp": 19,
    "ArrowDown": 20,
    "Home": 3,
    "End": 123,
    "PageUp": 92,
    "PageDown": 93,
    " ": 62,
    "Menu": 82,
    "Back": 4,
    "AppSwitch": 187,
}

# How far (in reported pixels) a press may move before it is treated as a swipe.
_TAP_SLOP = 12


AdbRunner = Callable[[list[str], bool], bytes]


class AdbError(RuntimeError):
    pass


def default_adb_runner(adb: str = "adb", serial: str | None = None) -> AdbRunner:
    prefix = [adb] + (["-s", serial] if serial else [])

    def run(args: list[str], binary: bool = False) -> bytes:
        try:
            proc = subprocess.run(
                prefix + args,
                capture_output=True,
                check=True,
                timeout=15,
            )
        except FileNotFoundError as exc:  # adb not installed
            raise AdbError(f"找不到 adb：{exc}") from exc
        except subprocess.CalledProcessError as exc:
            raise AdbError(exc.stderr.decode("utf-8", "replace").strip() or "adb 命令失败") from exc
        except subprocess.TimeoutExpired as exc:
            raise AdbError("adb 命令超时") from exc
        return proc.stdout if binary else proc.stdout

    return run


def list_devices(runner: AdbRunner) -> list[str]:
    out = runner(["devices"], False).decode("utf-8", "replace")
    devices: list[str] = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line or "offline" in line or "unauthorized" in line:
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def parse_wm_size(text: str) -> tuple[int, int] | None:
    # Prefer an Override size (the effective resolution) over Physical size.
    override = re.search(r"Override size:\s*(\d+)x(\d+)", text)
    physical = re.search(r"Physical size:\s*(\d+)x(\d+)", text)
    match = override or physical
    if not match:
        return None
    return int(match.group(1)), int(match.group(2))


class AndroidAdbSource:
    """Frame source and input sink backed by an ADB-connected Android device."""

    def __init__(
        self,
        *,
        serial: str | None = None,
        adb: str = "adb",
        runner: AdbRunner | None = None,
        max_width: int = 1080,
    ) -> None:
        from PIL import Image

        self._Image = Image
        self._max_width = max_width
        self._serial = serial
        self._run = runner or default_adb_runner(adb, serial)

        size = self._query_size()
        self.device_w, self.device_h = size
        # Reported (viewer-facing) size; updated on every grab after resizing.
        self.width, self.height = size
        self._press: tuple[int, int] | None = None

    # -- setup ---------------------------------------------------------------
    def _query_size(self) -> tuple[int, int]:
        try:
            out = self._run(["shell", "wm", "size"], False).decode("utf-8", "replace")
            size = parse_wm_size(out)
            if size:
                return size
        except AdbError as exc:
            logger.warning("读取屏幕尺寸失败：%s", exc)
        return (1080, 1920)

    # -- capture -------------------------------------------------------------
    def grab_image(self):
        png = self._run(["exec-out", "screencap", "-p"], True)
        img = self._Image.open(io.BytesIO(png)).convert("RGB")
        self.device_w, self.device_h = img.size
        if img.width > self._max_width:
            ratio = self._max_width / img.width
            img = img.resize((self._max_width, max(1, int(img.height * ratio))))
        self.width, self.height = img.size
        return img

    def grab_jpeg(self, quality: int = 70) -> bytes:
        buf = io.BytesIO()
        self.grab_image().save(buf, format="JPEG", quality=quality, optimize=True)
        return buf.getvalue()

    # -- coordinate mapping --------------------------------------------------
    def _to_device(self, x: int, y: int) -> tuple[int, int]:
        rw = self.width or self.device_w
        rh = self.height or self.device_h
        dx = x * self.device_w / rw
        dy = y * self.device_h / rh
        return int(round(dx)), int(round(dy))

    # -- input ---------------------------------------------------------------
    def handle_mouse(self, event: str, x: int, y: int, button: str = "left") -> None:
        dx, dy = self._to_device(x, y)
        if event == "down":
            self._press = (dx, dy)
        elif event == "up":
            start = self._press or (dx, dy)
            self._press = None
            if abs(dx - start[0]) <= _TAP_SLOP and abs(dy - start[1]) <= _TAP_SLOP:
                self._shell("input", "tap", str(dx), str(dy))
            else:
                self._shell("input", "swipe", str(start[0]), str(start[1]), str(dx), str(dy), "200")
        elif event == "scroll":
            delta = 400 if button == "up" else -400
            self._shell("input", "swipe", str(dx), str(dy), str(dx), str(dy - delta), "160")
        # bare "move" without a press is a no-op on touch devices

    def handle_key(self, event: str, key: str) -> None:
        if event != "down":  # avoid double input on key repeat/up
            return
        code = _KEYEVENTS.get(key)
        if code is not None:
            self._shell("input", "keyevent", str(code))
        elif len(key) == 1:
            self._shell("input", "text", "%s" if key == " " else key)

    def _shell(self, *args: str) -> None:
        try:
            self._run(["shell", *args], False)
        except AdbError as exc:
            logger.debug("adb input failed: %s", exc)

    def backend_name(self) -> str:
        return "android"

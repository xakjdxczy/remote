"""Tests for cross-platform host backends (desktop / android selection)."""

from __future__ import annotations

import io

import pytest
from PIL import Image

from remote.host import capture
from remote.host.android import (
    AndroidAdbSource,
    list_devices,
    parse_wm_size,
)
from remote.host.desktop import DesktopSource


# --------------------------------------------------------------------------- #
# Backend selection
# --------------------------------------------------------------------------- #
def test_open_frame_source_virtual():
    src = capture.open_frame_source(backend="virtual")
    assert src.backend_name() == "virtual"


def test_open_frame_source_prefer_virtual_alias():
    src = capture.open_frame_source(prefer_virtual=True)
    assert src.backend_name() == "virtual"


def test_open_frame_source_unknown_backend():
    with pytest.raises(ValueError):
        capture.open_frame_source(backend="nope")


def test_auto_falls_back_to_virtual_without_display(monkeypatch):
    monkeypatch.setattr(capture, "has_display", lambda: False)
    src = capture.open_frame_source(backend="auto")
    assert src.backend_name() == "virtual"


def test_auto_uses_desktop_when_display(monkeypatch):
    monkeypatch.setattr(capture, "has_display", lambda: True)
    sentinel = object()
    monkeypatch.setattr(capture, "_open_desktop", lambda max_width: sentinel)
    assert capture.open_frame_source(backend="auto") is sentinel


def test_desktop_backend_falls_back_to_virtual_on_error(monkeypatch):
    monkeypatch.setattr(capture, "_open_desktop", lambda max_width: None)
    src = capture.open_frame_source(backend="desktop")
    assert src.backend_name() == "virtual"


# --------------------------------------------------------------------------- #
# DesktopSource: capture + coordinate/input mapping (all injected)
# --------------------------------------------------------------------------- #
class _FakeGrab:
    def __init__(self, w, h):
        self.size = (w, h)
        self.bgra = bytes(w * h * 4)


class _FakeMouse:
    def __init__(self):
        self.position = (0, 0)
        self.events = []

    def press(self, btn):
        self.events.append(("press", btn, self.position))

    def release(self, btn):
        self.events.append(("release", btn, self.position))

    def scroll(self, dx, dy):
        self.events.append(("scroll", dx, dy))


class _FakeKeyboard:
    def __init__(self):
        self.events = []

    def press(self, key):
        self.events.append(("press", key))

    def release(self, key):
        self.events.append(("release", key))


def _desktop(monitor, **kw):
    grabber = lambda mon: _FakeGrab(mon["width"], mon["height"])  # noqa: E731
    return DesktopSource(
        grabber=grabber,
        monitor=monitor,
        mouse=_FakeMouse(),
        keyboard=_FakeKeyboard(),
        **kw,
    )


def test_desktop_grab_produces_jpeg_and_reports_size():
    src = _desktop({"left": 0, "top": 0, "width": 800, "height": 600}, max_width=1280)
    data = src.grab_jpeg(quality=60)
    img = Image.open(io.BytesIO(data))
    assert img.format == "JPEG"
    assert (src.width, src.height) == (800, 600)


def test_desktop_grab_downscales_wide_screen():
    src = _desktop({"left": 0, "top": 0, "width": 2560, "height": 1440}, max_width=1280)
    src.grab_jpeg()
    assert src.width == 1280
    assert src.height == 720


def test_desktop_coordinate_mapping_with_offset_and_scale():
    # 2560x1440 logical, displayed at 1280x720 -> factor 2, plus a monitor offset.
    src = _desktop({"left": 100, "top": 50, "width": 2560, "height": 1440}, max_width=1280)
    src.grab_jpeg()  # sets reported size to 1280x720
    src.handle_mouse("down", 640, 360, "left")  # center of the reported image
    # center maps to logical center (1280,720) plus offset (100,50)
    assert src._mouse.position == (100 + 1280, 50 + 720)
    assert src._mouse.events[-1][0] == "press"


def test_desktop_scroll_and_key_mapping():
    src = _desktop({"left": 0, "top": 0, "width": 800, "height": 600})
    src.handle_mouse("scroll", 10, 10, "up")
    assert src._mouse.events[-1] == ("scroll", 0, 1)
    src.handle_key("down", "a")
    assert src._keyboard.events[-1] == ("press", "a")
    src.handle_key("down", "Enter")  # special key -> resolved (headless returns name)
    assert src._keyboard.events[-1][0] == "press"
    # unknown multi-char key is ignored
    before = len(src._keyboard.events)
    src.handle_key("down", "F13xyz")
    assert len(src._keyboard.events) == before


# --------------------------------------------------------------------------- #
# AndroidAdbSource: command construction (fake adb runner)
# --------------------------------------------------------------------------- #
def _png_bytes(w, h):
    buf = io.BytesIO()
    Image.new("RGB", (w, h), (10, 20, 30)).save(buf, format="PNG")
    return buf.getvalue()


class _FakeAdb:
    def __init__(self, w=1080, h=1920):
        self.calls = []
        self._w, self._h = w, h

    def __call__(self, args, binary=False):
        self.calls.append(args)
        if args[:3] == ["shell", "wm", "size"]:
            return f"Physical size: {self._w}x{self._h}\n".encode()
        if args[:2] == ["exec-out", "screencap"]:
            return _png_bytes(self._w, self._h)
        if args == ["devices"]:
            return b"List of devices attached\nEMULATOR1\tdevice\nBROKEN\toffline\n"
        return b""

    def shell_calls(self):
        return [c[1:] for c in self.calls if c and c[0] == "shell"]


def test_parse_wm_size_prefers_override():
    assert parse_wm_size("Physical size: 1080x1920\nOverride size: 720x1280") == (720, 1280)
    assert parse_wm_size("Physical size: 1440x3200") == (1440, 3200)
    assert parse_wm_size("nonsense") is None


def test_list_devices_skips_offline_and_unauthorized():
    runner = _FakeAdb()
    assert list_devices(runner) == ["EMULATOR1"]


def test_android_reads_size_and_captures_jpeg():
    runner = _FakeAdb(w=1080, h=1920)
    src = AndroidAdbSource(runner=runner, max_width=540)
    assert (src.device_w, src.device_h) == (1080, 1920)
    data = src.grab_jpeg()
    assert Image.open(io.BytesIO(data)).format == "JPEG"
    assert src.width == 540  # downscaled
    assert src.height == 960


def test_android_tap_vs_swipe():
    runner = _FakeAdb(w=1000, h=2000)
    src = AndroidAdbSource(runner=runner, max_width=1000)  # 1:1 mapping
    src.grab_jpeg()
    # A press+release at (almost) the same point -> tap (at the release point)
    src.handle_mouse("down", 100, 200, "left")
    src.handle_mouse("up", 103, 202, "left")
    assert ["input", "tap", "103", "202"] in src._run_shell_log()
    # A press then release far away -> swipe
    src.handle_mouse("down", 100, 200, "left")
    src.handle_mouse("up", 600, 900, "left")
    assert ["input", "swipe", "100", "200", "600", "900", "200"] in src._run_shell_log()


def test_android_scroll_and_keys():
    runner = _FakeAdb(w=1000, h=2000)
    src = AndroidAdbSource(runner=runner, max_width=1000)
    src.grab_jpeg()
    src.handle_mouse("scroll", 500, 1000, "up")
    assert any(c[0] == "input" and c[1] == "swipe" for c in src._run_shell_log())
    src.handle_key("down", "a")
    assert ["input", "text", "a"] in src._run_shell_log()
    src.handle_key("down", "Enter")
    assert ["input", "keyevent", "66"] in src._run_shell_log()
    # keyup is ignored to avoid double input
    n = len(src._run_shell_log())
    src.handle_key("up", "a")
    assert len(src._run_shell_log()) == n


# helper hooked onto the source for assertions
def _run_shell_log(self):
    return [c[1:] for c in self._run.calls if c and c[0] == "shell"]


AndroidAdbSource._run_shell_log = _run_shell_log

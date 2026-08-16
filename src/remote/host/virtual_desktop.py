"""A drawn desktop used when no real display is available (headless / demo)."""

from __future__ import annotations

import io
import time
from dataclasses import dataclass, field

from PIL import Image, ImageDraw, ImageFont

WIDTH = 1280
HEIGHT = 720
TASKBAR_H = 48


@dataclass
class Window:
    title: str
    x: int
    y: int
    w: int
    h: int
    kind: str = "notepad"
    text: str = "欢迎使用 RemoteDesk\n\n这是演示桌面。你可以：\n• 拖动这扇窗口\n• 在这里远程打字\n• 点击左下角开始菜单\n"
    dragging: bool = False
    drag_dx: int = 0
    drag_dy: int = 0


@dataclass
class VirtualDesktop:
    width: int = WIDTH
    height: int = HEIGHT
    cursor_x: int = WIDTH // 2
    cursor_y: int = HEIGHT // 2
    start_open: bool = False
    mouse_down: bool = False
    windows: list[Window] = field(default_factory=list)
    keys_down: set[str] = field(default_factory=set)

    def __post_init__(self) -> None:
        if not self.windows:
            self.windows.append(Window("记事本", 220, 110, 720, 420))

    @property
    def focused(self) -> Window | None:
        return self.windows[-1] if self.windows else None

    def handle_mouse(self, event: str, x: int, y: int, button: str = "left") -> None:
        self.cursor_x = max(0, min(self.width - 1, int(x)))
        self.cursor_y = max(0, min(self.height - 1, int(y)))
        if event == "move" and self.mouse_down:
            win = self.focused
            if win and win.dragging:
                win.x = self.cursor_x - win.drag_dx
                win.y = max(0, self.cursor_y - win.drag_dy)
        elif event == "down":
            self.mouse_down = True
            if self._in_start(self.cursor_x, self.cursor_y):
                self.start_open = not self.start_open
                return
            if self.start_open and not self._in_start_menu(self.cursor_x, self.cursor_y):
                self.start_open = False
            win = self._hit_window(self.cursor_x, self.cursor_y)
            if win:
                self.windows.remove(win)
                self.windows.append(win)
                if self.cursor_y <= win.y + 36:
                    win.dragging = True
                    win.drag_dx = self.cursor_x - win.x
                    win.drag_dy = self.cursor_y - win.y
        elif event == "up":
            self.mouse_down = False
            if self.focused:
                self.focused.dragging = False

    def handle_key(self, event: str, key: str) -> None:
        if event == "down":
            self.keys_down.add(key)
            win = self.focused
            if not win or win.kind != "notepad":
                return
            if key == "Backspace":
                win.text = win.text[:-1]
            elif key == "Enter":
                win.text += "\n"
            elif key == "Tab":
                win.text += "    "
            elif len(key) == 1:
                win.text += key
        elif event == "up":
            self.keys_down.discard(key)

    def render_jpeg(self, quality: int = 70) -> bytes:
        buf = io.BytesIO()
        self.render_image().save(buf, format="JPEG", quality=quality, optimize=True)
        return buf.getvalue()

    def render_image(self) -> "Image.Image":
        img = Image.new("RGB", (self.width, self.height), (18, 36, 68))
        draw = ImageDraw.Draw(img)
        font = ImageFont.load_default()

        # Wallpaper gradient
        for y in range(self.height - TASKBAR_H):
            t = y / max(1, self.height - TASKBAR_H)
            r = int(18 + (46 - 18) * t)
            g = int(48 + (110 - 48) * t)
            b = int(92 + (168 - 92) * t)
            draw.line([(0, y), (self.width, y)], fill=(r, g, b))

        # Desktop icons
        icons = [("本机", 48, 40), ("文档", 48, 140), ("网络", 48, 240)]
        for label, ix, iy in icons:
            draw.rounded_rectangle((ix, iy, ix + 56, iy + 56), 8, fill=(255, 255, 255, ))
            draw.rectangle((ix + 10, iy + 14, ix + 46, iy + 46), fill=(47, 128, 237))
            draw.text((ix + 8, iy + 62), label, fill=(240, 246, 255), font=font)

        for win in self.windows:
            self._draw_window(draw, win, font, focused=win is self.focused)

        # Taskbar
        draw.rectangle((0, self.height - TASKBAR_H, self.width, self.height), fill=(20, 28, 44))
        start_bg = (47, 128, 237) if self.start_open else (36, 52, 80)
        draw.rounded_rectangle((10, self.height - 40, 96, self.height - 8), 8, fill=start_bg)
        draw.text((28, self.height - 32), "开始", fill=(255, 255, 255), font=font)
        clock = time.strftime("%H:%M:%S")
        date = time.strftime("%Y-%m-%d")
        draw.text((self.width - 110, self.height - 36), clock, fill=(230, 236, 245), font=font)
        draw.text((self.width - 118, self.height - 20), date, fill=(160, 176, 196), font=font)
        draw.text((120, self.height - 30), "RemoteDesk 演示桌面", fill=(180, 196, 214), font=font)

        if self.start_open:
            mx, my, mw, mh = 10, self.height - TASKBAR_H - 220, 240, 210
            draw.rounded_rectangle((mx, my, mx + mw, my + mh), 10, fill=(28, 38, 58))
            items = ["远程控制", "文件传输", "设置", "断开连接"]
            for i, item in enumerate(items):
                draw.text((mx + 24, my + 20 + i * 40), item, fill=(230, 236, 245), font=font)

        # Cursor
        self._draw_cursor(draw, self.cursor_x, self.cursor_y)

        return img

    def _draw_window(self, draw: ImageDraw.ImageDraw, win: Window, font, focused: bool) -> None:
        shadow = (0, 0, 0)
        draw.rounded_rectangle((win.x + 4, win.y + 6, win.x + win.w + 4, win.y + win.h + 6), 10, fill=shadow)
        body = (248, 250, 252)
        title_bg = (47, 128, 237) if focused else (90, 108, 130)
        draw.rounded_rectangle((win.x, win.y, win.x + win.w, win.y + win.h), 10, fill=body)
        draw.rectangle((win.x, win.y, win.x + win.w, win.y + 36), fill=title_bg)
        draw.text((win.x + 14, win.y + 10), win.title, fill=(255, 255, 255), font=font)
        draw.ellipse((win.x + win.w - 28, win.y + 10, win.x + win.w - 12, win.y + 26), fill=(240, 80, 80))
        # notepad content
        lines = win.text.split("\n")[-16:]
        ty = win.y + 52
        for line in lines:
            draw.text((win.x + 18, ty), line[:80], fill=(30, 40, 55), font=font)
            ty += 16
            if ty > win.y + win.h - 16:
                break

    def _draw_cursor(self, draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
        pts = [(x, y), (x, y + 18), (x + 5, y + 14), (x + 10, y + 22), (x + 13, y + 20), (x + 8, y + 12), (x + 16, y + 12)]
        draw.polygon(pts, fill=(255, 255, 255), outline=(0, 0, 0))

    def _hit_window(self, x: int, y: int) -> Window | None:
        for win in reversed(self.windows):
            if win.x <= x <= win.x + win.w and win.y <= y <= win.y + win.h:
                return win
        return None

    def _in_start(self, x: int, y: int) -> bool:
        return 10 <= x <= 96 and self.height - 40 <= y <= self.height - 8

    def _in_start_menu(self, x: int, y: int) -> bool:
        return 10 <= x <= 250 and self.height - TASKBAR_H - 220 <= y <= self.height - TASKBAR_H

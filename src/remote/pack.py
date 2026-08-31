"""Build a double-clickable desktop app (no Python install for the user)."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main() -> None:
    try:
        import PyInstaller  # noqa: F401
    except ImportError as exc:
        raise SystemExit("缺少打包工具。请安装：pip install '.[pack]'") from exc

    root = repo_root()
    spec = root / "desktop" / "dustx.spec"
    if not spec.is_file():
        raise SystemExit(f"找不到打包配置：{spec}")

    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", str(spec)]
    print("Packing DustX desktop app (user double-clicks; no Python install)...", flush=True)
    subprocess.check_call(cmd, cwd=root)

    if sys.platform == "darwin":
        built = root / "dist" / "尘埃.app"
        dest = root / "desktop" / "mac" / "尘埃.app"
    else:
        built = root / "dist" / "DustX"
        dest = root / "desktop" / "windows" / "DustX"
        if not built.exists():
            built = root / "dist" / "尘埃X"
            dest = root / "desktop" / "windows" / "尘埃X"
        if not built.exists():
            built = root / "dist" / "DustX.exe"
            dest = root / "desktop" / "windows" / "DustX.exe"

    if not built.exists():
        raise SystemExit("打包结束但没有找到输出文件")

    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        if dest.is_dir():
            shutil.rmtree(dest)
        else:
            dest.unlink()
    if built.is_dir():
        shutil.copytree(built, dest)
    else:
        shutil.copy2(built, dest)

    print(f"Built: {dest}", flush=True)
    print("Double-click to open. Python is not required.", flush=True)

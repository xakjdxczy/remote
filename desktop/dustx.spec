# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller spec: double-clickable 尘埃X.app / 尘埃X.exe."""

import sys
from pathlib import Path

from PyInstaller.utils.hooks import collect_all, collect_submodules

SPECDIR = Path(SPECPATH).resolve()
ROOT = SPECDIR.parent
ENTRY = SPECDIR / "entry.py"
WEB = ROOT / "src" / "remote" / "web"

datas = [(str(WEB), "remote/web")]
binaries = []
hiddenimports = collect_submodules("uvicorn") + [
    "remote.cam_sink",
    "remote.virtualio",
    "webview",
    "webview.platforms.cocoa",
    "webview.platforms.winforms",
    "webview.platforms.edgechromium",
]

for pkg in ("webview", "aiortc", "av", "sounddevice", "pyvirtualcam"):
    try:
        collected_datas, collected_binaries, collected_hidden = collect_all(pkg)
    except Exception:
        continue
    datas += collected_datas
    binaries += collected_binaries
    hiddenimports += collected_hidden

a = Analysis(
    [str(ENTRY)],
    pathex=[str(ROOT / "src")],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=["pytest", "tkinter"],
    noarchive=False,
)

pyz = PYZ(a.pure)

# Windows PyInstaller often breaks on non-ASCII exe names.
APP_NAME = "尘埃X" if sys.platform == "darwin" else "DustX"

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name=APP_NAME,
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=False,
    name=APP_NAME,
)

if sys.platform == "darwin":
    app = BUNDLE(
        coll,
        name="尘埃X.app",
        icon=None,
        bundle_identifier="com.dustx.remotedesk",
        info_plist={
            "CFBundleName": "尘埃X",
            "CFBundleDisplayName": "尘埃X",
            "NSHighResolutionCapable": True,
            "NSCameraUsageDescription": "预览手机作为本机摄像头时的画面",
            "NSMicrophoneUsageDescription": "使用手机麦克风作为本机输入",
        },
    )

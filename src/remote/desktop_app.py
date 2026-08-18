"""DustX desktop window for macOS and Windows.

The shipped product is a frozen ``尘埃X.app`` / ``尘埃X.exe``. Users double-click
that file. ``python -m remote app`` is only for source debugging.
"""

from __future__ import annotations

import logging
import os
import threading
import time
import urllib.error
import urllib.request

logger = logging.getLogger("remotedesk.desktop")


def _wait_health(port: int, tries: int = 80) -> None:
    url = f"http://127.0.0.1:{port}/api/health"
    last: Exception | None = None
    for _ in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=0.4) as resp:
                if resp.status == 200:
                    return
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last = exc
            time.sleep(0.1)
    raise RuntimeError(f"桌面服务未能在 :{port} 启动") from last


def run_app(host: str = "0.0.0.0", port: int = 8080) -> None:
    os.environ["DUSTX_DESKTOP"] = "1"
    import uvicorn
    from remote.server.api import app

    config = uvicorn.Config(app, host=host, port=port, log_level="info")
    server = uvicorn.Server(config)
    worker = threading.Thread(target=server.run, name="dustx-server", daemon=True)
    worker.start()
    _wait_health(port)

    try:
        import webview
    except ImportError as exc:
        raise SystemExit(
            "缺少桌面窗口组件。请安装：pip install pywebview"
        ) from exc

    window = webview.create_window(
        "尘埃X",
        f"http://127.0.0.1:{port}/",
        width=1280,
        height=860,
        min_size=(960, 640),
    )
    webview.start()
    server.should_exit = True
    try:
        window.destroy()
    except Exception:
        pass

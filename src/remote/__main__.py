"""CLI: remotedesk server | host | demo."""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
import threading
import time


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="remotedesk", description="ToDesk-style remote assistance")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_server = sub.add_parser("server", help="run the signaling server + web UI")
    p_server.add_argument("--host", default="0.0.0.0")
    p_server.add_argument("--port", type=int, default=8080)

    p_host = sub.add_parser("host", help="run the controlled-side agent")
    p_host.add_argument("--server", default=os.environ.get("REMOTEDESK_SERVER", "ws://127.0.0.1:8080/ws"))
    p_host.add_argument("--fps", type=int, default=12)
    p_host.add_argument("--quality", type=int, default=70)
    p_host.add_argument(
        "--backend",
        choices=["auto", "desktop", "android", "virtual"],
        default="auto",
        help="controlled-side backend (auto picks desktop when a display exists)",
    )
    p_host.add_argument("--adb-serial", default=None, help="target Android device serial for --backend android")
    p_host.add_argument("--virtual", action="store_true", help="alias for --backend virtual")

    p_demo = sub.add_parser("demo", help="start signaling + a local demo host together")
    p_demo.add_argument("--host", default="0.0.0.0")
    p_demo.add_argument("--port", type=int, default=8080)
    p_demo.add_argument("--fps", type=int, default=10)

    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")

    if args.cmd == "server":
        _run_server(args.host, args.port)
    elif args.cmd == "host":
        from remote.host.agent import main as host_main

        host_argv = [
            "--server", args.server,
            "--fps", str(args.fps),
            "--quality", str(args.quality),
            "--backend", args.backend,
        ]
        if args.adb_serial:
            host_argv += ["--adb-serial", args.adb_serial]
        if args.virtual:
            host_argv.append("--virtual")
        host_main(host_argv)
    elif args.cmd == "demo":
        _run_demo(args.host, args.port, args.fps)


def _run_server(host: str, port: int) -> None:
    import uvicorn
    from remote.server.api import app

    print(f"RemoteDesk 信令已启动: http://{host}:{port} （画面仅 P2P）", flush=True)
    uvicorn.run(app, host=host, port=port, log_level="info")


def _run_demo(host: str, port: int, fps: int) -> None:
    import uvicorn
    from remote.host.agent import HostAgent
    import remote.server.api as server_app

    def on_registered(device_id: str, password: str) -> None:
        server_app.demo_host = {"device_id": device_id, "password": password}

    agent = HostAgent(
        server=f"ws://127.0.0.1:{port}/ws",
        fps=fps,
        prefer_virtual=True,
        on_registered=on_registered,
    )

    def host_thread() -> None:
        time.sleep(0.8)
        asyncio.run(agent.run_forever())

    threading.Thread(target=host_thread, name="host-agent", daemon=True).start()
    print(f"RemoteDesk 演示模式: 打开 http://127.0.0.1:{port} 即可远程控制本机演示桌面", flush=True)
    uvicorn.run(server_app.app, host=host, port=port, log_level="info")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n已退出", file=sys.stderr)

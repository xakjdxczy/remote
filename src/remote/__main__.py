"""CLI: remotedesk server | host | demo."""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
import threading
import tempfile
import time
from pathlib import Path


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="remotedesk", description="ToDesk-style remote assistance")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_server = sub.add_parser("server", help="run the signaling server + web UI")
    p_server.add_argument("--host", default="0.0.0.0")
    p_server.add_argument("--port", type=int, default=8080)

    p_host = sub.add_parser("host", help="run the controlled-side agent")
    p_host.add_argument("--server", default=os.environ.get("REMOTEDESK_SERVER", "ws://127.0.0.1:8080/ws"))
    p_host.add_argument("--fps", type=int, default=30)
    p_host.add_argument("--quality", type=int, default=70)
    p_host.add_argument(
        "--backend",
        choices=["auto", "desktop", "android", "virtual"],
        default="auto",
        help="controlled-side backend (auto picks desktop when a display exists)",
    )
    p_host.add_argument("--adb-serial", default=None, help="target Android device serial for --backend android")
    p_host.add_argument("--virtual", action="store_true", help="alias for --backend virtual")
    p_host.add_argument("--auto-accept", action="store_true", help="skip incoming-call prompt")

    p_demo = sub.add_parser("demo", help="start signaling + a local demo host together")
    p_demo.add_argument("--host", default="0.0.0.0")
    p_demo.add_argument("--port", type=int, default=8080)
    p_demo.add_argument("--fps", type=int, default=30)

    p_upload = sub.add_parser("upload-apk", help="upload the Android APK to OSS")
    p_upload.add_argument("apk", help="path to the compiled .apk")
    p_upload.add_argument("--version", default="", help="versionName shown to downloaders")
    p_upload.add_argument("--version-code", default="", dest="version_code", help="integer versionCode")
    p_upload.add_argument(
        "--presign-put",
        action="store_true",
        help="print a time-limited PUT URL for uploading from another machine",
    )
    p_upload.add_argument("--expires", type=int, default=7200, help="presigned PUT lifetime in seconds")

    p_dl = sub.add_parser("upload-download", help="upload a desktop/android package to OSS")
    p_dl.add_argument("kind", choices=["android", "macos", "windows"])
    p_dl.add_argument("path", help="file, .app, or unpacked folder")
    p_dl.add_argument("--version", default="")
    p_dl.add_argument("--version-code", default="", dest="version_code")

    p_app = sub.add_parser("app", help="open the DustX desktop window (macOS / Windows)")
    p_app.add_argument("--host", default="0.0.0.0")
    p_app.add_argument("--port", type=int, default=8080)

    p_cam = sub.add_parser("cam-sink", help="(dev) feed phone camera/mic into virtual devices")
    p_cam.add_argument("--url", default="ws://127.0.0.1:8080/cam/ws")
    p_cam.add_argument("--token", required=True)

    sub.add_parser("pack", help="build 尘埃X.app / 尘埃X.exe (user does not run Python)")

    p_mesh = sub.add_parser("mesh", help="跨网互访打洞：WebRTC 隧道 + 本机端口转发")
    p_mesh.add_argument("--server", default=os.environ.get("REMOTEDESK_SERVER") or "")
    p_mesh.add_argument("--device", required=True)
    p_mesh.add_argument("--password", required=True)
    p_mesh.add_argument("--listen", type=int, default=2222)
    p_mesh.add_argument("--bind", default="127.0.0.1")
    p_mesh.add_argument("--service-port", type=int, default=22)
    p_mesh.add_argument("--name", default="")

    p_agent = sub.add_parser("agent", help="应用层协议：经 VPS 对对端 list/read/write/exec（不占 P2P）")
    p_agent.add_argument("--server", default=os.environ.get("REMOTEDESK_HTTP") or "")
    p_agent.add_argument("--device", required=True)
    p_agent.add_argument("--password", required=True)
    p_agent.add_argument("op", choices=["list", "read", "write", "exec"])
    p_agent.add_argument("--path", default="")
    p_agent.add_argument("--content", default="")
    p_agent.add_argument("--cwd", default="")
    p_agent.add_argument("--command", default="")
    p_agent.add_argument("extra", nargs=argparse.REMAINDER)

    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")

    if args.cmd == "app":
        from remote.desktop_app import run_app

        run_app(args.host, args.port)
    elif args.cmd == "server":
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
        if getattr(args, "auto_accept", False):
            host_argv.append("--auto-accept")
        host_main(host_argv)
    elif args.cmd == "demo":
        _run_demo(args.host, args.port, args.fps)
    elif args.cmd == "upload-apk":
        _upload_apk(args.apk, args.version, args.version_code, args.presign_put, args.expires)
    elif args.cmd == "upload-download":
        _upload_download(args.kind, args.path, args.version, args.version_code)
    elif args.cmd == "cam-sink":
        from remote.cam_sink import main as cam_sink_main

        cam_sink_main(["--url", args.url, "--token", args.token])
    elif args.cmd == "pack":
        from remote.pack import main as pack_main

        pack_main()
    elif args.cmd == "mesh":
        from remote.mesh_client import main as mesh_main
        from remote.urls import official_ws

        mesh_argv = [
            "--server", args.server or official_ws(),
            "--device", args.device,
            "--password", args.password,
            "--listen", str(args.listen),
            "--bind", args.bind,
            "--service-port", str(args.service_port),
        ]
        if args.name:
            mesh_argv += ["--name", args.name]
        raise SystemExit(mesh_main(mesh_argv))
    elif args.cmd == "agent":
        from remote.agent_cli import main as agent_main
        from remote.urls import official_http

        agent_argv = [
            "--server", args.server or official_http(),
            "--device", args.device,
            "--password", args.password,
            args.op,
            "--path", args.path,
            "--content", args.content,
            "--cwd", args.cwd,
        ]
        if args.command:
            agent_argv += ["--command", args.command]
        if args.extra:
            agent_argv += args.extra
        raise SystemExit(agent_main(agent_argv))


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
        auto_accept=True,
    )

    def host_thread() -> None:
        time.sleep(0.8)
        asyncio.run(agent.run_forever())

    threading.Thread(target=host_thread, name="host-agent", daemon=True).start()
    print(f"RemoteDesk 演示模式: 打开 http://127.0.0.1:{port} 即可远程控制本机演示桌面", flush=True)
    uvicorn.run(server_app.app, host=host, port=port, log_level="info")


def _upload_apk(apk: str, version: str, version_code: str, presign_put: bool, expires: int) -> None:
    if presign_put:
        from remote.server.oss import apk_put_instructions

        info = apk_put_instructions(apk, version=version, version_code=version_code, expires=expires)
        print(f"对象 {info['key']}  ({info['size']} bytes, sha256={info['sha256'][:12]}…)", flush=True)
        print(f"请在能访问 OSS 的电脑上执行（{info['expires_in']} 秒内有效）：", flush=True)
        print(info["curl"], flush=True)
        return

    from remote.server.oss import upload_apk

    info = upload_apk(apk, version=version, version_code=version_code)
    print(
        f"已上传 {info['filename']}  ({info['size']} bytes, sha256={info['sha256'][:12]}…)",
        flush=True,
    )
    if info.get("version"):
        print(f"版本 {info['version']} ({info.get('version_code') or '-'})", flush=True)


def _upload_download(kind: str, path: str, version: str, version_code: str) -> None:
    from remote.server.oss import MACOS_FILENAME, WINDOWS_FILENAME, archive_for_upload, upload_download

    names = {"macos": MACOS_FILENAME, "windows": WINDOWS_FILENAME, "android": "remotedesk-android.apk"}
    src = Path(path)
    staged = Path(tempfile.gettempdir()) / names.get(kind, src.name)
    packaged = archive_for_upload(src, staged)
    ephemeral = packaged != src
    try:
        info = upload_download(kind, packaged, version=version, version_code=version_code)
    finally:
        if ephemeral:
            packaged.unlink(missing_ok=True)
    print(
        f"已上传 {info['filename']}  ({info['size']} bytes, sha256={info['sha256'][:12]}…)",
        flush=True,
    )
    if info.get("version"):
        print(f"版本 {info['version']}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n已退出", file=sys.stderr)

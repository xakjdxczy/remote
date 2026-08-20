"""Cloud / CLI client for the application-layer agent protocol."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from typing import Any

from remote.ids import normalize_device_id
from remote.urls import official_http

MAX_CONTENT = 1 << 20


def agent_request(server: str, device_id: str, password: str, payload: dict[str, Any], timeout: float = 70) -> dict[str, Any]:
    body = {
        "device_id": normalize_device_id(device_id),
        "password": password,
        **payload,
    }
    url = server.rstrip("/") + "/api/agent"
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
            data = json.loads(raw)
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", "replace")
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            return {"ok": False, "error": raw or str(exc)}
        if isinstance(data, dict):
            if "error" not in data and "detail" in data:
                data = {"ok": False, "error": str(data.get("detail") or exc)}
            data.setdefault("ok", False)
            return data
        return {"ok": False, "error": str(exc)}
    except urllib.error.URLError as exc:
        return {"ok": False, "error": str(exc.reason or exc)}
    if not isinstance(data, dict):
        return {"ok": False, "error": "invalid response"}
    return data


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="尘埃X 应用层协议（不打洞，经 VPS 信令）")
    p.add_argument("--server", default=official_http(), help="信令 HTTP 源，默认官网")
    p.add_argument("--device", required=True, help="对端 9 位识别码")
    p.add_argument("--password", required=True, help="对端互访密码")
    p.add_argument("op", choices=["list", "read", "write", "exec"])
    p.add_argument("--path", default="", help="list/read/write 路径，相对用户主目录")
    p.add_argument("--content", default="", help="write 的文本内容")
    p.add_argument("--cwd", default="", help="exec 工作目录")
    p.add_argument("--command", default="", help="exec 命令；也可用 -- 后面的参数")
    p.add_argument("extra", nargs=argparse.REMAINDER, help="exec 命令（写在 -- 后面）")
    return p


def _command(args: argparse.Namespace) -> str:
    extra = list(args.extra or [])
    if extra and extra[0] == "--":
        extra = extra[1:]
    if extra:
        return " ".join(extra)
    return str(args.command or "")


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.op == "write" and len(args.content.encode("utf-8")) > MAX_CONTENT:
        print("content too large", file=sys.stderr)
        return 1
    payload: dict[str, Any] = {"op": args.op, "path": args.path, "cwd": args.cwd}
    if args.op == "write":
        payload["content"] = args.content
    if args.op == "exec":
        payload["command"] = _command(args)
        if not payload["command"]:
            print("exec 需要 --command 或 -- 后面的命令", file=sys.stderr)
            return 1
    data = agent_request(args.server, args.device, args.password, payload)
    if not data.get("ok"):
        print(data.get("error") or json.dumps(data, ensure_ascii=False), file=sys.stderr)
        return 1
    if args.op == "exec":
        if data.get("stdout"):
            sys.stdout.write(str(data["stdout"]))
            if not str(data["stdout"]).endswith("\n"):
                sys.stdout.write("\n")
        if data.get("stderr"):
            sys.stderr.write(str(data["stderr"]))
            if not str(data["stderr"]).endswith("\n"):
                sys.stderr.write("\n")
        code = data.get("exit")
        return int(code) if isinstance(code, int) else 0
    if args.op == "read":
        sys.stdout.write(str(data.get("content") or ""))
        if data.get("content") and not str(data["content"]).endswith("\n"):
            sys.stdout.write("\n")
        return 0
    print(json.dumps(data, ensure_ascii=False, indent=2))
    return 0

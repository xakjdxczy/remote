"""Official signaling URLs and ws ↔ http conversion."""

from __future__ import annotations

import os

OFFICIAL_WS = "wss://loessx.com/ws"
OFFICIAL_HTTP = "https://loessx.com"


def official_ws() -> str:
    return (os.environ.get("REMOTEDESK_SERVER") or OFFICIAL_WS).strip()


def ws_to_http(ws_url: str) -> str:
    url = (ws_url or "").strip()
    if url.startswith("wss://"):
        url = "https://" + url[6:]
    elif url.startswith("ws://"):
        url = "http://" + url[5:]
    cut = url.find("/ws")
    if cut >= 0:
        url = url[:cut]
    return url.rstrip("/")


def official_http() -> str:
    env = (os.environ.get("REMOTEDESK_HTTP") or "").strip()
    if env:
        return env.rstrip("/")
    return ws_to_http(official_ws()) or OFFICIAL_HTTP

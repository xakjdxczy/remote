"""Shared wire protocol for host, viewer, and relay."""

from __future__ import annotations

import json
import struct
from enum import IntEnum
from typing import Any

PROTOCOL_VERSION = 1


class BinaryType(IntEnum):
    FRAME = 1
    FILE_CHUNK = 2


# Binary frame: type(u8) + width(u16) + height(u16) + ts_ms(u32) + jpeg
FRAME_HEADER = struct.Struct("!BHH I")
# Binary file chunk: type(u8) + transfer_id(u32) + offset(u64) + payload
FILE_HEADER = struct.Struct("!BIQ")


def encode_json(payload: dict[str, Any]) -> str:
    payload.setdefault("v", PROTOCOL_VERSION)
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def decode_json(raw: str) -> dict[str, Any]:
    data = json.loads(raw)
    if not isinstance(data, dict):
        raise ValueError("JSON payload must be an object")
    return data


def pack_frame(jpeg: bytes, width: int, height: int, ts_ms: int) -> bytes:
    return FRAME_HEADER.pack(BinaryType.FRAME, width, height, ts_ms) + jpeg


def unpack_frame(data: bytes) -> tuple[int, int, int, bytes]:
    if len(data) < FRAME_HEADER.size:
        raise ValueError("frame too short")
    kind, width, height, ts_ms = FRAME_HEADER.unpack_from(data)
    if kind != BinaryType.FRAME:
        raise ValueError(f"not a frame: {kind}")
    return width, height, ts_ms, data[FRAME_HEADER.size :]


def pack_file_chunk(transfer_id: int, offset: int, payload: bytes) -> bytes:
    return FILE_HEADER.pack(BinaryType.FILE_CHUNK, transfer_id, offset) + payload


def unpack_file_chunk(data: bytes) -> tuple[int, int, bytes]:
    if len(data) < FILE_HEADER.size:
        raise ValueError("file chunk too short")
    kind, transfer_id, offset = FILE_HEADER.unpack_from(data)
    if kind != BinaryType.FILE_CHUNK:
        raise ValueError(f"not a file chunk: {kind}")
    return transfer_id, offset, data[FILE_HEADER.size :]


def peek_binary_type(data: bytes) -> int:
    if not data:
        raise ValueError("empty binary message")
    return data[0]

from remote.protocol import (
    BinaryType,
    decode_json,
    encode_json,
    pack_file_chunk,
    pack_frame,
    peek_binary_type,
    unpack_file_chunk,
    unpack_frame,
)


def test_json_roundtrip_adds_version():
    raw = encode_json({"type": "ping", "t": 1})
    msg = decode_json(raw)
    assert msg["type"] == "ping"
    assert msg["v"] == 1


def test_frame_roundtrip():
    payload = pack_frame(b"jpeg-bytes", 1280, 720, 12345)
    assert peek_binary_type(payload) == BinaryType.FRAME
    width, height, ts, jpeg = unpack_frame(payload)
    assert (width, height, ts, jpeg) == (1280, 720, 12345, b"jpeg-bytes")


def test_file_chunk_roundtrip():
    payload = pack_file_chunk(7, 4096, b"abc")
    assert peek_binary_type(payload) == BinaryType.FILE_CHUNK
    transfer_id, offset, data = unpack_file_chunk(payload)
    assert (transfer_id, offset, data) == (7, 4096, b"abc")

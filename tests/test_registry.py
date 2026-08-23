import pytest

from remote.ids import is_usable_temp_password
from remote.server.registry import Registry


class DummyWs:
    pass


def test_register_and_lookup():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="secret12")
    assert host.device_id == "111222333"
    assert registry.get("111 222 333") is host
    assert registry.lookup_by_ws(host.ws) is host


def test_register_replaces_mask_password():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="••••••••")
    assert host.temp_password != "••••••••"
    assert is_usable_temp_password(host.temp_password)
    assert not registry.set_password(host.device_id, "------")
    assert is_usable_temp_password(host.temp_password)


def test_session_busy_and_end():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "Linux", temp_password="pw")
    viewer = DummyWs()
    session = registry.create_session(host, viewer, "bob")
    registry.accept(session.session_id)
    with pytest.raises(RuntimeError, match="busy"):
        registry.create_session(host, DummyWs(), "carol")
    ended = registry.end_session(session.session_id)
    assert ended.session_id == session.session_id
    assert host.session_id is None
    again = registry.create_session(host, DummyWs(), "dave")
    assert again.session_id != session.session_id


def test_preferred_id_is_reclaimed_not_rotated():
    registry = Registry()
    first = DummyWs()
    host = registry.register_host(first, "pc-a", "Linux", preferred_id="111222333", temp_password="keepme12")
    assert host.device_id == "111222333"
    second = DummyWs()
    again = registry.register_host(second, "pc-a", "Linux", preferred_id="111222333", temp_password="keepme12")
    assert again.device_id == "111222333"
    assert registry.lookup_by_ws(first) is None
    assert registry.lookup_by_ws(second) is again
    assert again.temp_password == "keepme12"


def test_mesh_host_allows_two_viewers():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "macOS", temp_password="pw")
    first = registry.create_session(host, DummyWs(), "尘埃X-mesh")
    second = registry.create_session(host, DummyWs(), "尘埃X-mesh")
    assert first.session_id != second.session_id
    assert host.session_ids == [first.session_id, second.session_id]
    registry.end_session(first.session_id)
    assert host.session_id == second.session_id
    assert host.session_ids == [second.session_id]


def test_mesh_and_remote_share_one_code():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "macOS", temp_password="pw")
    mesh = registry.create_session(host, DummyWs(), "尘埃X-mesh")
    remote = registry.create_session(host, DummyWs(), "alice")
    assert mesh.session_id != remote.session_id
    assert host.session_ids == [mesh.session_id, remote.session_id]
    with pytest.raises(RuntimeError, match="busy"):
        registry.create_session(host, DummyWs(), "bob")


def test_mesh_caps_at_eight():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "macOS", temp_password="pw")
    for _ in range(8):
        registry.create_session(host, DummyWs(), "尘埃X-mesh")
    with pytest.raises(RuntimeError, match="busy"):
        registry.create_session(host, DummyWs(), "尘埃X-mesh")


def test_unregister_ends_session():
    registry = Registry()
    host_ws = DummyWs()
    host = registry.register_host(host_ws, "pc-a", "Linux")
    registry.create_session(host, DummyWs(), "bob")
    dropped = registry.unregister(host_ws)
    assert len(dropped) == 1
    assert registry.get(host.device_id) is None
    assert registry.stats() == {"hosts": 0, "sessions": 0, "total_sessions": 1, "issued": 1, "machines": 0}


def test_issued_id_not_reused_after_offline():
    registry = Registry()
    first = registry.register_host(DummyWs(), "pc-a", "Linux")
    offline = first.device_id
    registry.unregister(first.ws)
    second = registry.register_host(DummyWs(), "pc-b", "Linux")
    assert second.device_id != offline
    assert registry.ids.contains(offline)
    assert registry.stats()["issued"] == 2


def test_offline_reconnect_keeps_password_when_client_sends_empty():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="keepme12")
    registry.unregister(host.ws)
    assert registry.get("111222333") is None
    again = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="")
    assert again.temp_password == "keepme12"
    masked = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="••••••••")
    assert masked.temp_password == "keepme12"
    other = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="changed1")
    assert other.temp_password == "keepme12"


def test_password_survives_process_restart(tmp_path):
    from remote.server.device_db import DeviceDB

    db = DeviceDB(tmp_path / "devices.sqlite")
    first = Registry(hw=db)
    host = first.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="keepme12")
    assert host.temp_password == "keepme12"
    first.unregister(host.ws)

    restarted = Registry(hw=DeviceDB(tmp_path / "devices.sqlite"))
    again = restarted.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="")
    assert again.temp_password == "keepme12"
    rotated = restarted.refresh_password(again.device_id)
    assert rotated != "keepme12"

    third = Registry(hw=DeviceDB(tmp_path / "devices.sqlite"))
    back = third.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="localfile")
    assert back.temp_password == rotated


def test_refresh_password_is_the_only_rotation():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="keepme12")
    rotated = registry.refresh_password(host.device_id)
    assert rotated != "keepme12"
    assert registry.refresh_password(host.device_id) == rotated
    registry.unregister(host.ws)
    again = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="")
    assert again.temp_password == rotated


def test_preferred_id_stays_reserved_while_offline():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333")
    registry.unregister(host.ws)
    other = registry.register_host(DummyWs(), "pc-b", "Linux")
    assert other.device_id != "111222333"
    back = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333")
    assert back.device_id == "111222333"


def test_id_store_survives_process_restart(tmp_path):
    from remote.server.id_store import AllocatedIds

    path = tmp_path / "device_ids.txt"
    first = Registry(ids=AllocatedIds(path))
    a = first.register_host(DummyWs(), "pc-a", "Linux")
    b = first.register_host(DummyWs(), "pc-b", "Linux", preferred_id="999888777")
    first.unregister(a.ws)

    restarted = Registry(ids=AllocatedIds(path))
    fresh = restarted.register_host(DummyWs(), "pc-c", "Linux")
    assert fresh.device_id not in {a.device_id, "999888777"}
    assert restarted.ids.contains(a.device_id)
    again = restarted.register_host(DummyWs(), "pc-b", "Linux", preferred_id="999888777")
    assert again.device_id == "999888777"
    assert b.device_id == "999888777"


def _fp(board="BOARD1", nic="aa:bb:cc:dd:ee:ff", uuid="12345678-1234-1234-1234-123456789abc"):
    return {"board": board, "nic": nic, "uuid": uuid}


def test_fingerprint_keeps_same_code_after_reinstall():
    from remote.server.device_db import DeviceDB

    registry = Registry(hw=DeviceDB(None))
    first = registry.register_host(DummyWs(), "pc-a", "macOS", fingerprint=_fp())
    code = first.device_id
    registry.unregister(first.ws)
    again = registry.register_host(DummyWs(), "pc-a", "macOS", fingerprint=_fp())
    assert again.device_id == code
    assert registry.stats()["machines"] == 1


def test_fingerprint_wins_over_stale_preferred_id():
    from remote.server.device_db import DeviceDB

    registry = Registry(hw=DeviceDB(None))
    first = registry.register_host(DummyWs(), "pc-a", "macOS", fingerprint=_fp())
    code = first.device_id
    registry.unregister(first.ws)
    again = registry.register_host(
        DummyWs(), "pc-a", "macOS", preferred_id="111222333", fingerprint=_fp()
    )
    assert again.device_id == code


def test_same_preferred_id_new_hardware_gets_new_code():
    from remote.server.device_db import DeviceDB

    registry = Registry(hw=DeviceDB(None))
    a = registry.register_host(DummyWs(), "pc-a", "macOS", preferred_id="111222333", fingerprint=_fp())
    assert a.device_id == "111222333"
    registry.unregister(a.ws)
    b = registry.register_host(
        DummyWs(),
        "pc-b",
        "Windows",
        preferred_id="111222333",
        fingerprint=_fp(board="BOARD2", nic="11:22:33:44:55:66", uuid="abcdefab-cdef-abcd-efab-cdefabcdefab"),
    )
    assert b.device_id != "111222333"
    back = registry.register_host(DummyWs(), "pc-a", "macOS", fingerprint=_fp())
    assert back.device_id == "111222333"


def test_device_table_survives_restart(tmp_path):
    from remote.server.device_db import DeviceDB

    path = tmp_path / "devices.sqlite"
    first = Registry(hw=DeviceDB(path))
    host = first.register_host(DummyWs(), "pc-a", "macOS", fingerprint=_fp())
    code = host.device_id
    restarted = Registry(hw=DeviceDB(path))
    again = restarted.register_host(DummyWs(), "pc-a", "macOS", fingerprint=_fp())
    assert again.device_id == code
    assert restarted.hw.count() == 1

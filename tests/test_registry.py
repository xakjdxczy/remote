import pytest

from remote.server.registry import Registry


class DummyWs:
    pass


def test_register_and_lookup():
    registry = Registry()
    host = registry.register_host(DummyWs(), "pc-a", "Linux", preferred_id="111222333", temp_password="secret12")
    assert host.device_id == "111222333"
    assert registry.get("111 222 333") is host
    assert registry.lookup_by_ws(host.ws) is host


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


def test_unregister_ends_session():
    registry = Registry()
    host_ws = DummyWs()
    host = registry.register_host(host_ws, "pc-a", "Linux")
    registry.create_session(host, DummyWs(), "bob")
    dropped = registry.unregister(host_ws)
    assert len(dropped) == 1
    assert registry.get(host.device_id) is None
    assert registry.stats() == {"hosts": 0, "sessions": 0, "total_sessions": 1}

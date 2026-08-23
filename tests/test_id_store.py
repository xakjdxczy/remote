from remote.server.id_store import AllocatedIds, attach_store, resolve_id_store_path
from remote.server.registry import Registry


def test_resolve_memory_and_explicit_path(monkeypatch, tmp_path):
    monkeypatch.setenv("DUSTX_ID_STORE", "memory")
    assert resolve_id_store_path() is None
    monkeypatch.setenv("DUSTX_ID_STORE", str(tmp_path / "ids.txt"))
    assert resolve_id_store_path() == tmp_path / "ids.txt"
    monkeypatch.delenv("DUSTX_ID_STORE")
    monkeypatch.setenv("DUSTX_DATA_DIR", str(tmp_path / "data"))
    assert resolve_id_store_path() == tmp_path / "data" / "device_ids.txt"


def test_attach_store_writes_file(monkeypatch, tmp_path):
    path = tmp_path / "device_ids.txt"
    monkeypatch.setenv("DUSTX_ID_STORE", str(path))
    registry = Registry()
    host = registry.register_host(object(), "pc-a", "Linux")
    attach_store(registry)
    assert path.is_file()
    assert host.device_id in path.read_text(encoding="utf-8")
    assert registry.ids.path == path
    again = AllocatedIds(path)
    assert again.contains(host.device_id)

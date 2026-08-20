import os

from remote.agent_ops import resolve_agent_path, run_agent


def test_agent_ops_roundtrip(tmp_path, monkeypatch):
    monkeypatch.setenv("DUSTX_AGENT_ROOT", str(tmp_path))
    (tmp_path / "note.txt").write_text("hello", encoding="utf-8")
    listed = run_agent("list", path="")
    assert listed["ok"] is True
    names = {e["name"] for e in listed["entries"]}
    assert "note.txt" in names

    read = run_agent("read", path="note.txt")
    assert read["ok"] is True
    assert read["content"] == "hello"

    wrote = run_agent("write", path="sub/a.txt", content="world")
    assert wrote["ok"] is True
    assert (tmp_path / "sub" / "a.txt").read_text(encoding="utf-8") == "world"

    ran = run_agent("exec", command="echo dustx-agent")
    assert ran["ok"] is True
    assert ran["exit"] == 0
    assert "dustx-agent" in ran["stdout"]


def test_agent_ops_rejects_escape(tmp_path, monkeypatch):
    monkeypatch.setenv("DUSTX_AGENT_ROOT", str(tmp_path))
    outside = tmp_path.parent / "outside.txt"
    outside.write_text("nope", encoding="utf-8")
    assert run_agent("read", path="../" + outside.name)["ok"] is False
    assert run_agent("read", path=str(outside))["ok"] is False
    assert run_agent("list", path=str(tmp_path.parent))["ok"] is False


def test_resolve_stays_in_root(tmp_path, monkeypatch):
    monkeypatch.setenv("DUSTX_AGENT_ROOT", str(tmp_path))
    inside = resolve_agent_path("ok.txt")
    assert inside == tmp_path / "ok.txt"
    try:
        resolve_agent_path("../nope")
        assert False, "should reject"
    except ValueError:
        pass


def test_unknown_op():
    assert run_agent("delete")["ok"] is False

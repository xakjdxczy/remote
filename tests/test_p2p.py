from remote.p2p import ice_servers_payload, maybe_strip_relay, relay_enabled, strip_relay_sdp


def test_ice_servers_are_stun_only_by_default(monkeypatch):
    monkeypatch.delenv("TURN_URLS", raising=False)
    servers = ice_servers_payload()
    assert servers
    assert not relay_enabled()
    for server in servers:
        for url in server["urls"]:
            assert url.startswith("stun:")
            assert not url.startswith("turn:")


def test_turn_added_when_configured(monkeypatch):
    monkeypatch.setenv("TURN_URLS", "turn:1.2.3.4:3478?transport=udp,turn:1.2.3.4:3478?transport=tcp")
    monkeypatch.setenv("TURN_USER", "rd")
    monkeypatch.setenv("TURN_PASS", "secret")
    assert relay_enabled()
    servers = ice_servers_payload()
    turn = [s for s in servers if "username" in s]
    assert turn, "expected a TURN server entry"
    assert turn[0]["username"] == "rd"
    assert turn[0]["credential"] == "secret"
    assert all(u.startswith("turn:") for u in turn[0]["urls"])


def test_maybe_strip_relay_depends_on_turn(monkeypatch):
    sdp = "\r\n".join(
        [
            "v=0",
            "a=candidate:1 1 UDP 2122260223 192.168.1.2 9 typ host",
            "a=candidate:3 1 UDP 16721920 5.6.7.8 3478 typ relay raddr 1.2.3.4 rport 9",
            "",
        ]
    )
    monkeypatch.delenv("TURN_URLS", raising=False)
    assert "typ relay" not in maybe_strip_relay(sdp)  # pure P2P strips relay
    monkeypatch.setenv("TURN_URLS", "turn:1.2.3.4:3478")
    monkeypatch.setenv("TURN_USER", "rd")
    monkeypatch.setenv("TURN_PASS", "secret")
    assert "typ relay" in maybe_strip_relay(sdp)  # TURN enabled keeps relay


def test_strip_relay_candidates():
    sdp = "\r\n".join(
        [
            "v=0",
            "a=candidate:1 1 UDP 2122260223 192.168.1.2 9 typ host",
            "a=candidate:2 1 UDP 1677729535 1.2.3.4 9 typ srflx raddr 192.168.1.2 rport 9",
            "a=candidate:3 1 UDP 16721920 5.6.7.8 3478 typ relay raddr 1.2.3.4 rport 9",
            "",
        ]
    )
    cleaned = strip_relay_sdp(sdp)
    assert "typ host" in cleaned
    assert "typ srflx" in cleaned
    assert "typ relay" not in cleaned

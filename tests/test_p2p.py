from remote.p2p import ice_servers_payload, strip_relay_sdp


def test_ice_servers_are_stun_only():
    servers = ice_servers_payload()
    assert servers
    for server in servers:
        for url in server["urls"]:
            assert url.startswith("stun:")
            assert not url.startswith("turn:")


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

from remote.device_fp import parse_fingerprint


def test_parse_requires_all_three():
    assert parse_fingerprint({"board": "SN123", "nic": "aa:bb:cc:dd:ee:ff"}) is None
    assert parse_fingerprint({"board": "SN123", "uuid": "12345678-1234-1234-1234-123456789abc"}) is None
    fp = parse_fingerprint({
        "board": "sn-123",
        "nic": "AA-BB-CC-DD-EE-FF",
        "uuid": "{12345678-1234-1234-1234-123456789ABC}",
    })
    assert fp is not None
    assert fp.board == "SN-123"
    assert fp.nic == "aabbccddeeff"
    assert fp.sys_uuid == "12345678123412341234123456789abc"
    assert fp.complete


def test_placeholders_rejected():
    assert parse_fingerprint({
        "board": "To Be Filled By O.E.M.",
        "nic": "00:00:00:00:00:00",
        "uuid": "00000000-0000-0000-0000-000000000000",
    }) is None


def test_same_hardware_same_key():
    a = parse_fingerprint({"board": "X", "nic": "aa:bb:cc:dd:ee:01", "uuid": "a" * 32})
    b = parse_fingerprint({"board": "x", "nic": "AABBCCDDEE01", "uuid": "A" * 32})
    assert a is not None and b is not None
    assert a.key == b.key
    other = parse_fingerprint({"board": "Y", "nic": "aa:bb:cc:dd:ee:01", "uuid": "a" * 32})
    assert other is not None
    assert other.key != a.key

from remote.ids import (
    constant_time_equals,
    format_device_id,
    generate_device_id,
    generate_temp_password,
    is_usable_temp_password,
    normalize_device_id,
)


def test_generate_device_id_unique_and_nine_digits():
    seen = set()
    for _ in range(20):
        device_id = generate_device_id(seen)
        assert len(device_id) == 9
        assert device_id.isdigit()
        assert device_id != "000000000"
        seen.add(device_id)
    assert len(seen) == 20


def test_format_and_normalize_roundtrip():
    assert format_device_id("123456789") == "123 456 789"
    assert normalize_device_id("123 456 789") == "123456789"


def test_password_length_and_compare():
    password = generate_temp_password(8)
    assert len(password) == 8
    assert constant_time_equals(password, password)
    assert not constant_time_equals(password, password.lower() + "x")


def test_mask_glyphs_are_not_usable_passwords():
    assert not is_usable_temp_password("")
    assert not is_usable_temp_password(None)
    assert not is_usable_temp_password("••••••••")
    assert not is_usable_temp_password("------")
    assert not is_usable_temp_password("........")
    assert is_usable_temp_password(generate_temp_password())
    assert is_usable_temp_password("abc12xyz")

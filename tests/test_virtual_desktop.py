from remote.host.virtual_desktop import VirtualDesktop
from remote.host.files import FileInbox


def test_virtual_desktop_renders_jpeg_and_accepts_input(tmp_path):
    desk = VirtualDesktop()
    first = desk.render_jpeg(quality=50)
    assert first[:2] == b"\xff\xd8"
    desk.handle_mouse("move", 300, 120)
    desk.handle_mouse("down", 300, 120)
    desk.handle_mouse("move", 340, 160)
    desk.handle_mouse("up", 340, 160)
    desk.handle_key("down", "A")
    desk.handle_key("down", "B")
    assert "AB" in desk.focused.text
    second = desk.render_jpeg(quality=50)
    assert len(second) > 100
    assert second != first


def test_start_menu_toggle():
    desk = VirtualDesktop()
    assert desk.start_open is False
    desk.handle_mouse("down", 40, desk.height - 24)
    assert desk.start_open is True
    desk.handle_mouse("down", 40, desk.height - 24)
    assert desk.start_open is False


def test_file_inbox(tmp_path):
    inbox = FileInbox(tmp_path)
    path = inbox.begin(1, "../evil/hello.txt", 5)
    assert path.name == "hello.txt"
    inbox.write(1, 0, b"hello")
    finished = inbox.finish(1)
    assert finished.read_bytes() == b"hello"

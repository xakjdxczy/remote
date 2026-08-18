from remote.virtualio import VirtualCamera, VirtualMic


def test_virtual_camera_missing_backend():
    cam = VirtualCamera()
    assert cam.open() is False
    cam.send_rgb(None)
    cam.close()


def test_virtual_mic_missing_backend():
    mic = VirtualMic()
    assert mic.open() is False
    mic.write(None)
    mic.close()

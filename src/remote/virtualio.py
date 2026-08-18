"""Optional virtual webcam / microphone sinks.

System devices need a signed driver or a third-party virtual device:

* Camera: ``pip install pyvirtualcam`` plus OBS Virtual Camera (Win/macOS)
* Mic: play into BlackHole (macOS) or VB-CABLE Input (Windows)

If those are missing, callers still get a local preview elsewhere.
"""

from __future__ import annotations

import logging
from typing import Any

logger = logging.getLogger("remotedesk.virtualio")


class VirtualCamera:
    def __init__(self, width: int = 1280, height: int = 720, fps: int = 30) -> None:
        self.width = width
        self.height = height
        self.fps = fps
        self._cam: Any = None

    def open(self) -> bool:
        try:
            import pyvirtualcam
        except ImportError:
            logger.info("pyvirtualcam not installed; skip virtual camera")
            return False
        try:
            self._cam = pyvirtualcam.Camera(
                width=self.width,
                height=self.height,
                fps=self.fps,
                fmt=pyvirtualcam.PixelFormat.RGB,
            )
            logger.info("virtual camera opened: %s", getattr(self._cam, "device", "dustx"))
            return True
        except Exception as exc:
            logger.warning("virtual camera open failed: %s", exc)
            self._cam = None
            return False

    def send_rgb(self, frame: Any) -> None:
        if self._cam is None:
            return
        try:
            self._cam.send(frame)
            self._cam.sleep_until_next_frame()
        except Exception as exc:
            logger.warning("virtual camera send failed: %s", exc)

    def close(self) -> None:
        cam = self._cam
        self._cam = None
        if cam is None:
            return
        try:
            cam.close()
        except Exception:
            pass


class VirtualMic:
    """Play received PCM into a virtual cable so other apps can pick it up as input."""

    CANDIDATE_NAMES = ("BlackHole", "CABLE Input", "VB-Audio", "DustX", "尘埃")

    def __init__(self, sample_rate: int = 48000, channels: int = 1) -> None:
        self.sample_rate = sample_rate
        self.channels = channels
        self._stream: Any = None
        self.device_name = ""

    def open(self) -> bool:
        try:
            import sounddevice as sd
        except ImportError:
            logger.info("sounddevice not installed; skip virtual mic")
            return False
        device = _find_output_device(sd)
        if device is None:
            logger.info("no virtual cable output found (BlackHole / VB-CABLE)")
            return False
        index, name = device
        try:
            self._stream = sd.OutputStream(
                samplerate=self.sample_rate,
                channels=self.channels,
                dtype="float32",
                device=index,
            )
            self._stream.start()
            self.device_name = name
            logger.info("virtual mic feeding %s", name)
            return True
        except Exception as exc:
            logger.warning("virtual mic open failed: %s", exc)
            self._stream = None
            return False

    def write(self, pcm_f32: Any) -> None:
        if self._stream is None:
            return
        try:
            self._stream.write(pcm_f32)
        except Exception as exc:
            logger.warning("virtual mic write failed: %s", exc)

    def close(self) -> None:
        stream = self._stream
        self._stream = None
        if stream is None:
            return
        try:
            stream.stop()
            stream.close()
        except Exception:
            pass


def _find_output_device(sd: Any) -> tuple[int, str] | None:
    try:
        devices = sd.query_devices()
    except Exception:
        return None
    for index, info in enumerate(devices):
        name = str(info.get("name") or "")
        if info.get("max_output_channels", 0) < 1:
            continue
        if any(key.lower() in name.lower() for key in VirtualMic.CANDIDATE_NAMES):
            return index, name
    return None

"""Shared, lazy Picamera2 owner for MJPEG streaming and still capture."""

import asyncio
import io
import logging
import threading
from collections.abc import AsyncIterator

logger = logging.getLogger("phenotype_edge.video_stream")


class VideoStream:
    def __init__(self) -> None:
        self._camera = None
        self._lock = threading.Lock()

    def _configure_video(self, camera) -> None:
        camera.configure(camera.create_video_configuration())
        camera.set_controls({"AfMode": 1, "AfTrigger": 0})
        camera.start()

    def _ensure_camera(self):
        if self._camera is None:
            from picamera2 import Picamera2  # noqa: PLC0415

            camera = Picamera2()
            self._configure_video(camera)
            self._camera = camera
            logger.info("Video camera initialized")
        return self._camera

    def initialize(self) -> None:
        with self._lock:
            self._ensure_camera()

    def next_part(self) -> bytes:
        with self._lock:
            camera = self._ensure_camera()
            frame = camera.capture_array()
            import cv2  # noqa: PLC0415

            frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
            encoded, buffer = cv2.imencode(".jpg", frame)
            if not encoded:
                raise RuntimeError("Failed to encode video frame as JPEG")
            return (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n"
                + buffer.tobytes()
                + b"\r\n"
            )

    def capture_still(self) -> bytes:
        """Capture one still, then restore the live-video configuration."""
        with self._lock:
            camera = self._ensure_camera()
            camera.stop()
            try:
                camera.configure(camera.create_still_configuration())
                camera.start()
                stream = io.BytesIO()
                camera.capture_file(stream, format="jpeg")
                return stream.getvalue()
            finally:
                camera.stop()
                self._configure_video(camera)

    def stop(self) -> None:
        with self._lock:
            if self._camera is None:
                return
            try:
                self._camera.stop()
            finally:
                self._camera = None


_stream = VideoStream()


def initialize() -> None:
    _stream.initialize()


def capture_still() -> bytes:
    return _stream.capture_still()


def stop() -> None:
    _stream.stop()


async def iter_mjpeg() -> AsyncIterator[bytes]:
    try:
        while True:
            yield await asyncio.to_thread(_stream.next_part)
    finally:
        logger.debug("MJPEG client disconnected")

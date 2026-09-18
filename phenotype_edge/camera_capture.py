"""Periodically capture and upload a JPEG image."""

import asyncio
import logging

import httpx

from . import video_stream
from .config import settings

logger = logging.getLogger("phenotype_edge.camera_capture")


class PiCameraCapturer:
    """Capture stills through the shared Pi camera owner."""

    def __init__(self) -> None:
        video_stream.initialize()

    def capture(self) -> bytes:
        return video_stream.capture_still()


class USBCameraCapturer:
    """Capture stills from a USB camera using the configured device index."""

    def __init__(self, device_index: int) -> None:
        import cv2  # noqa: PLC0415

        self._cv2 = cv2
        self.cap = cv2.VideoCapture(device_index)
        if not self.cap.isOpened():
            raise RuntimeError(f"Could not open camera device index {device_index}")

    def capture(self) -> bytes:
        ok, frame = self.cap.read()
        if not ok:
            raise RuntimeError("Failed to read a frame from the camera")
        ok, buffer = self._cv2.imencode(".jpg", frame)
        if not ok:
            raise RuntimeError("Failed to encode frame as JPEG")
        return buffer.tobytes()


def build_capturer():
    if settings.camera_backend == "picamera2":
        return PiCameraCapturer()
    if settings.camera_backend == "usb":
        return USBCameraCapturer(settings.camera_device_index)
    raise ValueError(f"Unknown camera_backend: {settings.camera_backend}")


async def upload_capture(client: httpx.AsyncClient, image_bytes: bytes) -> None:
    try:
        response = await client.post(
            f"{settings.main_backend_url}/ingest/camera-capture",
            headers={"X-API-Key": settings.ingest_api_key},
            data={"camera_id": str(settings.camera_id)},
            files={"file": ("capture.jpg", image_bytes, "image/jpeg")},
            timeout=settings.request_timeout_seconds,
        )
        response.raise_for_status()
        logger.info("Uploaded capture (%d bytes) for camera %d", len(image_bytes), settings.camera_id)
    except httpx.HTTPError as error:
        logger.warning("Failed to upload capture: %s", error)


async def run_forever() -> None:
    if not settings.camera_enabled:
        return

    try:
        capturer = build_capturer()
    except Exception as error:
        logger.error("Camera init failed (%s) -- capture loop disabled for this run", error)
        return

    async with httpx.AsyncClient() as client:
        while True:
            try:
                image_bytes = await asyncio.to_thread(capturer.capture)
                await upload_capture(client, image_bytes)
            except Exception as error:
                logger.warning("Capture cycle failed: %s", error)
            await asyncio.sleep(settings.capture_interval_seconds)

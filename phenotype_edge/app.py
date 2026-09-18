"""Unified FastAPI application for the Phenotyping Pi edge service."""

import asyncio
import logging
from contextlib import asynccontextmanager

import httpx
from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from pydantic import BaseModel

from . import camera_capture, dht_poller, video_stream
from .config import settings

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
logger = logging.getLogger("phenotype_edge.app")


@asynccontextmanager
async def lifespan(app: FastAPI):
    background_tasks = [asyncio.create_task(dht_poller.run_forever())]
    if settings.camera_enabled:
        background_tasks.append(asyncio.create_task(camera_capture.run_forever()))
    logger.info(
        "edge service started (relay_proxy=%s, camera=%s)",
        settings.relay_proxy_enabled,
        settings.camera_enabled,
    )
    try:
        yield
    finally:
        for task in background_tasks:
            task.cancel()
        await asyncio.gather(*background_tasks, return_exceptions=True)
        await asyncio.to_thread(video_stream.stop)


app = FastAPI(title="Phenotyping Pi Edge Service", lifespan=lifespan)


class RelayProxyIn(BaseModel):
    channel: str
    state: bool


@app.get("/health")
async def health():
    return {
        "status": "ok",
        "relay_proxy_enabled": settings.relay_proxy_enabled,
        "camera_enabled": settings.camera_enabled,
    }


@app.post("/relay-proxy")
async def relay_proxy(payload: RelayProxyIn):
    if not settings.relay_proxy_enabled:
        raise HTTPException(403, "Relay proxy disabled on this edge service instance")

    state_param = "on" if payload.state else "off"
    if payload.channel == "ac":
        inverted_state_param = "off" if payload.state else "on"
        action_url = f"http://{settings.relay_host}/ac?state={inverted_state_param}"
        confirm_key = "ac"
    else:
        action_url = f"http://{settings.relay_host}/relay?ch={payload.channel}&state={state_param}"
        confirm_key = f"relay{payload.channel}"

    try:
        async with httpx.AsyncClient(timeout=settings.request_timeout_seconds) as client:
            action_response = await client.get(action_url)
            action_response.raise_for_status()
            status_response = await client.get(f"http://{settings.relay_host}/status")
            status_response.raise_for_status()
        body = status_response.json()
    except httpx.HTTPError as error:
        raise HTTPException(502, f"Could not reach {settings.relay_host}: {error}")
    except ValueError:
        raise HTTPException(502, f"{settings.relay_host} returned a non-JSON response")

    if confirm_key not in body:
        raise HTTPException(502, f"{settings.relay_host}'s /status didn't include '{confirm_key}': {body}")

    confirmed_state = body[confirm_key]
    if payload.channel == "ac":
        confirmed_state = not confirmed_state
    return {
        "ok": confirmed_state == payload.state,
        "confirmed_state": confirmed_state,
        "device_response": body,
    }


@app.get("/video")
async def video():
    return StreamingResponse(
        video_stream.iter_mjpeg(),
        media_type="multipart/x-mixed-replace; boundary=frame",
    )

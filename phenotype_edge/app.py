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

logging.basicConfig(
    level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s"
)
logger = logging.getLogger("phenotype_edge.app")


@asynccontextmanager
async def relay_keepalive():
    """GET the relay board's /status once a minute.

    The relay firmware reboots itself after five minutes without serving a
    request (its way out of the wedged-but-associated state we keep seeing).
    Nothing else talks to that board routinely, so this is what keeps a
    healthy board from rebooting. Failures are logged and ignored.
    """
    while True:
        if settings.relay_proxy_enabled and settings.relay_host:
            try:
                async with httpx.AsyncClient(timeout=settings.request_timeout_seconds) as client:
                    await client.get(f"http://{settings.relay_host}/status")
            except httpx.HTTPError as error:
                logger.warning("relay keepalive: %s unreachable (%s)", settings.relay_host, error)
        await asyncio.sleep(60)


async def lifespan(app: FastAPI):
    background_tasks = [
        asyncio.create_task(dht_poller.run_forever()),
        asyncio.create_task(relay_keepalive()),
    ]
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
    host = settings.relay_host
    if payload.channel == "ac":
        inverted_state_param = "off" if payload.state else "on"
        action_url = f"http://{host}/ac?state={inverted_state_param}"
        confirm_key = "ac"
    elif payload.channel == "exhaust":
        if not settings.exhaust_fan_host:
            raise HTTPException(
                502, "EXHAUST_FAN_HOST is not configured on this edge service"
            )
        host = settings.exhaust_fan_host
        ch = settings.exhaust_fan_channel
        action_url = f"http://{host}/relay?ch={ch}&state={state_param}"
        confirm_key = f"relay{ch}"
    else:
        action_url = f"http://{host}/relay?ch={payload.channel}&state={state_param}"
        confirm_key = f"relay{payload.channel}"

    try:
        async with httpx.AsyncClient(
            timeout=settings.request_timeout_seconds
        ) as client:
            action_response = await client.get(action_url)
            action_response.raise_for_status()
            status_response = await client.get(f"http://{host}/status")
            status_response.raise_for_status()
        body = status_response.json()
    except httpx.HTTPError as error:
        raise HTTPException(502, f"Could not reach {host}: {error}")
    except ValueError:
        raise HTTPException(502, f"{host} returned a non-JSON response")

    if confirm_key not in body:
        raise HTTPException(
            502,
            f"{host}'s /status didn't include '{confirm_key}': {body}",
        )

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

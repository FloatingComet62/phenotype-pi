"""Unified FastAPI application for the Phenotyping Pi edge service."""

import asyncio
import logging
from contextlib import asynccontextmanager

import httpx
from fastapi import FastAPI, HTTPException
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field

from . import camera_capture, dht_poller, video_stream
from .config import settings
from .resolve import forget, resolve
from .stacks import brightness, led_segment, stack_host

logging.basicConfig(
    level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s"
)
logger = logging.getLogger("phenotype_edge.app")


async def relay_keepalive():
    """GET /status on the relay board and every KEEPALIVE_HOSTS board, once a minute.

    Their firmware restarts itself after five minutes without serving a
    request (its way out of the wedged-but-associated state). Nothing else
    talks to these boards routinely, so this is what keeps a healthy one
    from restarting. Failures are logged and ignored.
    """
    while True:
        hosts = [h.strip() for h in settings.keepalive_hosts.split(",") if h.strip()]
        if settings.relay_proxy_enabled and settings.relay_host:
            hosts.insert(0, settings.relay_host)
        for host in hosts:
            try:
                addr = await resolve(host)
                async with httpx.AsyncClient(timeout=settings.request_timeout_seconds) as client:
                    await client.get(f"http://{addr}/status")
            except httpx.HTTPError as error:
                logger.warning("keepalive: %s unreachable (%s)", host, error)
                forget(host)
        await asyncio.sleep(60)


@asynccontextmanager
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


def _client() -> httpx.AsyncClient:
    return httpx.AsyncClient(timeout=settings.request_timeout_seconds)


async def _get_json(client: httpx.AsyncClient, url: str) -> dict:
    response = await client.get(url)
    response.raise_for_status()
    return response.json()


async def _switch_valve(client, host: str, branch: str, want: bool) -> dict:
    """Open or close one branch valve, taking the master valve with it.

    Opening: master first, then the branch. Closing: the branch, then the
    master unless another branch is still open. Returns the board's final
    /status.
    """
    master = settings.valve_master_channel
    state = "on" if want else "off"
    if want:
        await _get_json(client, f"http://{host}/relay?ch={master}&state=on")
    body = await _get_json(client, f"http://{host}/relay?ch={branch}&state={state}")
    if not want:
        others_open = any(
            on
            for key, on in body.items()
            if key.startswith("relay") and key not in (f"relay{master}", f"relay{branch}")
        )
        if not others_open:
            await _get_json(client, f"http://{host}/relay?ch={master}&state=off")
    return await _get_json(client, f"http://{host}/status")


@app.post("/relay-proxy")
async def relay_proxy(payload: RelayProxyIn):
    """channel: "1".."4" raw relay | "ac" | "ac2" | "exhaust" | "valve1".."valve3"."""
    if not settings.relay_proxy_enabled:
        raise HTTPException(403, "Relay proxy disabled on this edge service instance")

    channel, want = payload.channel, payload.state
    state = "on" if want else "off"
    invert = False
    valve = None
    host_setting = settings.relay_host

    if channel == "ac":
        # Wired inverted on the relay board: output released = AC running.
        invert = True
        path, confirm_key = f"/ac?state={'off' if want else 'on'}", "ac"
    elif channel == "ac2":
        if not settings.ac2_host:
            raise HTTPException(502, "AC2_HOST is not configured on this edge service")
        host_setting = settings.ac2_host
        path, confirm_key = f"/ac?state={state}", "ac"
    elif channel == "exhaust":
        if not settings.exhaust_fan_host:
            raise HTTPException(
                502, "EXHAUST_FAN_HOST is not configured on this edge service"
            )
        host_setting = settings.exhaust_fan_host
        ch = settings.exhaust_fan_channel
        path, confirm_key = f"/relay?ch={ch}&state={state}", f"relay{ch}"
    elif channel.startswith("valve"):
        valve = channel[len("valve"):]
        if not settings.valve_master_channel:
            raise HTTPException(
                502, "VALVE_MASTER_CHANNEL is not configured on this edge service"
            )
        if not valve.isdigit() or valve == settings.valve_master_channel:
            raise HTTPException(400, f"'{channel}' is not a branch valve")
        path, confirm_key = None, f"relay{valve}"
    else:
        path, confirm_key = f"/relay?ch={channel}&state={state}", f"relay{channel}"

    host = await resolve(host_setting)
    try:
        async with _client() as client:
            if valve is not None:
                body = await _switch_valve(client, host, valve, want)
            else:
                await _get_json(client, f"http://{host}{path}")
                body = await _get_json(client, f"http://{host}/status")
    except httpx.HTTPError as error:
        forget(host_setting)
        raise HTTPException(502, f"Could not reach {host}: {error}")
    except ValueError:
        raise HTTPException(502, f"{host} returned a non-JSON response")

    if confirm_key not in body:
        raise HTTPException(
            502,
            f"{host}'s /status didn't include '{confirm_key}': {body}",
        )

    confirmed_state = (not body[confirm_key]) if invert else body[confirm_key]
    ok = confirmed_state == want
    if valve is not None and want:
        # An open branch is useless without the master.
        ok = ok and bool(body.get(f"relay{settings.valve_master_channel}"))
    return {"ok": ok, "confirmed_state": confirmed_state, "device_response": body}


class StackLedIn(BaseModel):
    stack: int
    shelf: int
    row: int
    r: int = Field(ge=0, le=255)
    g: int = Field(ge=0, le=255)
    b: int = Field(ge=0, le=255)
    intensity: int = Field(ge=0, le=100)


@app.post("/stack-led")
async def stack_led(payload: StackLedIn):
    """Set one row's LEDs. The bridge passes it to the stack's Arduino.

    `ok` means the bridge took the command. The Arduino never acknowledges,
    so `arduino_online` (is it sending telemetry?) is the only evidence that
    anyone was listening; the bridge sends the colour again when it returns.
    """
    host_name = stack_host(settings.stack_hosts, payload.stack)
    if host_name is None:
        raise HTTPException(404, f"Stack {payload.stack} has no bridge in STACK_HOSTS")
    segment = led_segment(payload.shelf, payload.row)
    if segment is None:
        raise HTTPException(
            404, f"Shelf {payload.shelf} row {payload.row} is not in the LED map"
        )
    path, start, end = segment
    query = (
        f"path={path}&start={start}&end={end}"
        f"&r={payload.r}&g={payload.g}&b={payload.b}"
        f"&brightness={brightness(payload.intensity)}"
    )
    host = await resolve(host_name)
    try:
        async with _client() as client:
            body = await _get_json(client, f"http://{host}/led?{query}")
    except httpx.HTTPError as error:
        forget(host_name)
        raise HTTPException(502, f"Could not reach {host_name} ({host}): {error}")
    except ValueError:
        raise HTTPException(502, f"{host_name} returned a non-JSON response")
    return {
        "ok": True,
        "arduino_online": bool(body.get("arduino_online")),
        "sent": {"path": path, "start": start, "end": end},
        "device_response": body,
    }


@app.get("/video")
async def video():
    return StreamingResponse(
        video_stream.iter_mjpeg(),
        media_type="multipart/x-mixed-replace; boundary=frame",
    )

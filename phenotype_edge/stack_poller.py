"""Tank pH from each stack's bridge to the backend.

Every poll interval, each bridge in STACK_HOSTS is asked for /reading. A
fresh pH goes to that stack's pH sensor on the backend. Being asked also
counts as traffic for the bridge, which restarts itself when idle.

The backend's stack sensors are not in GET /sensors (that list is the DHT
boards'), so the sensor is found through GET /farm (stacks in order) and
GET /v2/sensors (the pH sensor of that stack).
"""

import asyncio
import logging

import httpx

from .config import settings
from .resolve import forget, resolve

logger = logging.getLogger("stack_poller")

# A reading older than this is the Arduino's last words, not the tank's pH.
MAX_AGE_SECONDS = 10


def hosts() -> list[str]:
    return [h.strip() for h in settings.stack_hosts.split(",")]


def ph_sensor_ids(farm: dict, sensors: list[dict]) -> dict[int, int]:
    """stack position (from 1) -> id of that stack's pH sensor."""
    out = {}
    for position, stack in enumerate(farm.get("stacks", []), start=1):
        for sensor in sensors:
            if sensor.get("stack_id") == stack["id"] and sensor.get("sensor_type") == "ph":
                out[position] = sensor["id"]
    return out


def usable_ph(reading: dict) -> float | None:
    ph = reading.get("ph")
    age = reading.get("ph_age_seconds", -1)
    if ph is None or not 0 <= age <= MAX_AGE_SECONDS or not 0 <= ph <= 14:
        return None
    return ph


async def poll_once(client: httpx.AsyncClient, sensor_ids: dict[int, int]) -> int:
    readings = []
    for position, host in enumerate(hosts(), start=1):
        if not host or position not in sensor_ids:
            continue
        addr = await resolve(host)
        try:
            resp = await client.get(f"http://{addr}/reading", timeout=settings.request_timeout_seconds)
            if resp.status_code == 503:
                logger.info("%s: its Arduino is sending nothing", host)
                continue
            resp.raise_for_status()
            ph = usable_ph(resp.json())
        except (httpx.HTTPError, ValueError) as e:
            logger.warning("Failed to read %s (%s): %s", host, addr, e)
            forget(host)
            continue
        if ph is not None:
            readings.append({"sensor_id": sensor_ids[position], "value": ph})
    if not readings:
        return 0
    resp = await client.post(
        f"{settings.main_backend_url}/ingest/sensor-readings/batch",
        json={"readings": readings},
        headers={"X-API-Key": settings.ingest_api_key},
        timeout=settings.request_timeout_seconds,
    )
    resp.raise_for_status()
    dropped = resp.json().get("dropped_sensor_ids") or []
    if dropped:
        logger.warning("backend does not know pH sensor ids %s", dropped)
    logger.info("Posted %d pH reading(s)", len(readings) - len(dropped))
    return len(readings) - len(dropped)


async def resolve_sensor_ids(client: httpx.AsyncClient) -> dict[int, int]:
    farm = (await client.get(f"{settings.main_backend_url}/farm", timeout=20)).json()
    sensors = (await client.get(f"{settings.main_backend_url}/v2/sensors", timeout=20)).json()
    return ph_sensor_ids(farm, sensors)


async def run_forever() -> None:
    if not any(hosts()):
        logger.info("no STACK_HOSTS, stack poller idle")
        return
    sensor_ids: dict[int, int] = {}
    cycles = 0
    async with httpx.AsyncClient() as client:
        while True:
            try:
                if not sensor_ids or cycles % 30 == 0:
                    sensor_ids = await resolve_sensor_ids(client)
                    logger.info("pH sensors by stack: %s", sensor_ids)
                await poll_once(client, sensor_ids)
            except (httpx.HTTPError, ValueError, KeyError) as e:
                logger.warning("stack poll failed: %s", e)
            cycles += 1
            await asyncio.sleep(settings.dht_poll_interval_seconds)

"""Polls the 3 DHT nodes (dht1.local, dht2.local, dht3.local) and
forwards readings to the main backend's sensor-ingestion API. Runs as a
background asyncio task inside edge_service.py -- see run_forever() at
the bottom.

Resilience note: this runs unattended over a 4G link. Any single failure
(a DHT device unreachable, the main backend unreachable, a malformed
response) is logged and skipped -- it never crashes the loop. Losing one
reading is fine; losing the whole service until someone notices is not.
"""

import asyncio
import logging

import httpx

from .config import settings

logger = logging.getLogger("dht_poller")

# (host, "temperature"|"humidity") -> sensor_id in the main backend,
# resolved once at startup by resolve_sensor_ids().
_sensor_id_cache: dict[tuple[str, str], int] = {}


async def resolve_sensor_ids(client: httpx.AsyncClient) -> None:
    """Looks up sensor IDs by name (see register_hardware.py in the main
    project for the registration side). Zone number = position in
    DHT_HOSTS (Zone 1 = first host, Zone 2 = second, ...), not the
    hostname itself -- keeps the dashboard's labels human-friendly
    regardless of what the devices are actually called. Retries with
    backoff on startup in case the main backend isn't reachable yet."""
    delay = 5
    while True:
        try:
            resp = await client.get(f"{settings.main_backend_url}/sensors")
            resp.raise_for_status()
            all_sensors = resp.json()
            _sensor_id_cache.clear()
            for i, host in enumerate(settings.dht_host_list):
                zone = i + 1
                for kind, field_label in (("temperature", "Temperature"), ("humidity", "Humidity")):
                    name = f"Zone {zone} {field_label}"
                    match = next((s for s in all_sensors if s["name"] == name), None)
                    if match:
                        _sensor_id_cache[(host, kind)] = match["id"]
                    else:
                        logger.warning(
                            "No sensor named '%s' found on the backend -- "
                            "register it first (see register_hardware.py or seed_demo_data.py). "
                            "Readings for %s will be dropped until then.", name, host,
                        )
            logger.info("Resolved %d/%d DHT sensor IDs", len(_sensor_id_cache), len(settings.dht_host_list) * 2)
            return
        except httpx.HTTPError as e:
            logger.warning("Couldn't reach main backend to resolve sensor IDs (%s), retrying in %ds", e, delay)
            await asyncio.sleep(delay)
            delay = min(delay * 2, 60)


# dht1's firmware emits {"temp_c", "humidity"} while dht2 emits the
# canonical {"temperature_c", "humidity_percent"}. Both are healthy boards;
# only the spelling differs. The configured field name is still tried first,
# so this widens what we accept without changing what we prefer.
_TEMP_ALIASES = ("temperature_c", "temp_c", "temperature")
_HUM_ALIASES = ("humidity_percent", "humidity", "humidity_pct")


def _first_present(data: dict, configured: str, aliases: tuple[str, ...]):
    for key in (configured, *aliases):
        value = data.get(key)
        if value is not None:
            return value
    return None


async def poll_one_host(client: httpx.AsyncClient, host: str) -> dict | None:
    try:
        resp = await client.get(f"http://{host}{settings.dht_reading_path}", timeout=settings.request_timeout_seconds)
        resp.raise_for_status()
        return resp.json()
    except (httpx.HTTPError, ValueError) as e:
        logger.warning("Failed to read %s%s: %s", host, settings.dht_reading_path, e)
        return None
async def poll_once(client: httpx.AsyncClient) -> bool:
    """Returns True if the backend reported dropped/unknown sensor IDs
    this cycle -- the caller uses this to trigger an immediate
    re-resolve, so a backend DB reset self-heals within one poll cycle
    instead of requiring someone to notice and restart this service."""
    readings = []
    for host in settings.dht_host_list:
        data = await poll_one_host(client, host)
        if data is None:
            continue
        temp = _first_present(data, settings.dht_temp_field, _TEMP_ALIASES)
        hum = _first_present(data, settings.dht_humidity_field, _HUM_ALIASES)
        if temp is None and hum is None:
            # A 200 response with unrecognised keys used to be silent: the
            # HTTP call succeeded, data.get() returned None, and the reading
            # vanished with no log line. That hid dht1 for a month.
            logger.warning(
                "%s responded but carried no recognised temperature/humidity "
                "field (keys seen: %s)", host, sorted(data),
            )
        if temp is not None and (host, "temperature") in _sensor_id_cache:
            readings.append({"sensor_id": _sensor_id_cache[(host, "temperature")], "value": float(temp)})
        if hum is not None and (host, "humidity") in _sensor_id_cache:
            readings.append({"sensor_id": _sensor_id_cache[(host, "humidity")], "value": float(hum)})

    if not readings:
        logger.warning("No DHT readings collected this cycle")
        return False

    try:
        resp = await client.post(
            f"{settings.main_backend_url}/ingest/sensor-readings/batch",
            json={"readings": readings},
            headers={"X-API-Key": settings.ingest_api_key},
            timeout=settings.request_timeout_seconds,
        )
        resp.raise_for_status()
        body = resp.json()
        dropped = body.get("dropped_sensor_ids") or []
        if dropped:
            logger.warning(
                "Backend dropped %d reading(s) for unknown sensor_id(s) %s -- "
                "its DB was likely reset since we last resolved; re-resolving now",
                len(dropped), dropped,
            )
            return True
        logger.info("Posted %d DHT readings", body.get("count", len(readings)))
        return False
    except httpx.HTTPError as e:
        logger.warning("Failed to post DHT readings to main backend: %s", e)
        return False


# Belt-and-suspenders re-resolve even without a dropped-ID signal (e.g. if
# the backend was reset AND every one of our cached IDs happens to still
# exist by coincidence, or an older backend without dropped_sensor_ids in
# its response). Cheap, so no harm running it periodically regardless.
_RESOLVE_EVERY_N_CYCLES = 30


async def run_forever() -> None:
    async with httpx.AsyncClient() as client:
        await resolve_sensor_ids(client)
        cycles_since_resolve = 0
        while True:
            needs_resolve = await poll_once(client)
            cycles_since_resolve += 1
            if needs_resolve or cycles_since_resolve >= _RESOLVE_EVERY_N_CYCLES:
                await resolve_sensor_ids(client)
                cycles_since_resolve = 0
            await asyncio.sleep(settings.dht_poll_interval_seconds)
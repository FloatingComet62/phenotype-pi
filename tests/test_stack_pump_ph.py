"""Pump channel and pH poller, against a fake bridge and a fake backend."""
import asyncio

import httpx
import pytest

from phenotype_edge import app as edge
from phenotype_edge import stack_poller
from phenotype_edge.config import settings


def test_pump_on_is_the_configured_speed_and_off_is_zero(monkeypatch):
    seen, online = [], {"v": True}

    def handler(request):
        seen.append(str(request.url))
        return httpx.Response(200, json={
            "pump_speed": int(request.url.params["speed"]), "arduino_online": online["v"]})

    monkeypatch.setattr(edge, "_client", lambda: httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    monkeypatch.setattr(settings, "stack_hosts", "10.0.0.9")
    monkeypatch.setattr(settings, "pump_on_speed", 128)
    call = lambda ch, st: asyncio.run(edge.relay_proxy(edge.RelayProxyIn(channel=ch, state=st)))

    on = call("pump1", True)
    assert on["ok"] and on["confirmed_state"] is True and "warning" not in on
    off = call("pump1", False)
    assert off["ok"] and off["confirmed_state"] is False
    assert seen == ["http://10.0.0.9/pump?speed=128", "http://10.0.0.9/pump?speed=0"]

    online["v"] = False
    assert "not responding" in call("pump1", True)["warning"]

    seen.clear()
    with pytest.raises(edge.HTTPException) as e:
        call("pump2", True)
    assert e.value.status_code == 404 and seen == []


def test_ph_sensor_is_found_by_stack_order_not_by_name():
    farm = {"stacks": [{"id": 7, "name": "Left"}, {"id": 3, "name": "Right"}]}
    sensors = [
        {"id": 2203, "stack_id": 3, "sensor_type": "ph"},
        {"id": 2207, "stack_id": 7, "sensor_type": "ph"},
        {"id": 2307, "stack_id": 7, "sensor_type": "water_level"},
        {"id": 1, "stack_id": None, "sensor_type": "temperature"},
    ]
    assert stack_poller.ph_sensor_ids(farm, sensors) == {1: 2207, 2: 2203}


def test_only_a_fresh_plausible_ph_is_posted():
    ok = stack_poller.usable_ph
    assert ok({"ph": 6.84, "ph_age_seconds": 0}) == 6.84
    assert ok({"ph": 6.84, "ph_age_seconds": 60}) is None      # the Arduino went quiet
    assert ok({"ph": None, "ph_age_seconds": -1}) is None      # never heard from it
    assert ok({"ph": 14.9, "ph_age_seconds": 0}) is None       # probe unplugged


def test_poll_posts_to_the_right_sensor(monkeypatch):
    posted = []

    def handler(request):
        if request.url.path == "/reading":
            return httpx.Response(200, json={"ph": 6.5, "ph_age_seconds": 1})
        posted.append((request.headers["x-api-key"], request.read()))
        return httpx.Response(200, json={"count": 1, "dropped_sensor_ids": []})

    monkeypatch.setattr(settings, "stack_hosts", "10.0.0.9")
    monkeypatch.setattr(settings, "main_backend_url", "http://backend/api/v1")
    monkeypatch.setattr(settings, "ingest_api_key", "k")

    async def run():
        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as c:
            return await stack_poller.poll_once(c, {1: 2201})

    assert asyncio.run(run()) == 1
    assert posted == [("k", b'{"readings":[{"sensor_id":2201,"value":6.5}]}')] or b'"sensor_id": 2201' in posted[0][1]

"""Row -> strip segment mapping and the /stack-led route, against a fake bridge."""
import asyncio

import httpx
import pytest

from phenotype_edge import app as edge
from phenotype_edge.config import settings
from phenotype_edge.stacks import LED_MAP, brightness, stack_host


def test_map_is_the_one_measured_on_site():
    assert LED_MAP[(1, 1)] == (0, 4, 87)
    assert LED_MAP[(1, 4)] == (1, 1, 83)       # row 4s on pin 12
    assert LED_MAP[(2, 3)] == (2, 206, 290)    # upper shelf on pin 11
    assert LED_MAP[(2, 4)] == (1, 106, 188)
    assert len(LED_MAP) == 8


def test_rows_on_one_strip_never_overlap():
    by_path = {}
    for path, start, end in LED_MAP.values():
        assert 0 <= start <= end <= 299
        by_path.setdefault(path, []).append((start, end))
    for ranges in by_path.values():
        ranges.sort()
        assert all(a[1] < b[0] for a, b in zip(ranges, ranges[1:]))


def test_brightness_and_hosts():
    assert (brightness(0), brightness(50), brightness(100), brightness(140)) == (0, 128, 255, 255)
    assert stack_host("stack1.local, stack2.local", 2) == "stack2.local"
    assert stack_host("stack1.local", 2) is None and stack_host("", 1) is None


@pytest.fixture
def bridge(monkeypatch):
    seen = []

    def handler(request):
        seen.append(str(request.url))
        return httpx.Response(200, json={"arduino_online": False, "leds": []})

    monkeypatch.setattr(edge, "_client", lambda: httpx.AsyncClient(transport=httpx.MockTransport(handler)))
    monkeypatch.setattr(settings, "stack_hosts", "10.0.0.9")
    return seen


def send(**kw):
    body = dict(stack=1, shelf=1, row=2, r=255, g=60, b=200, intensity=80) | kw
    return asyncio.run(edge.stack_led(edge.StackLedIn(**body)))


def test_a_row_becomes_the_right_segment(bridge):
    out = send()
    assert bridge == ["http://10.0.0.9/led?path=0&start=104&end=190&r=204&g=48&b=160&brightness=255"]
    assert out["ok"] and out["arduino_online"] is False
    assert out["sent"] == {"path": 0, "start": 104, "end": 190}


def test_unknown_stack_or_row_is_refused_before_anything_is_sent(bridge):
    for bad in (dict(stack=2), dict(shelf=3), dict(row=5)):
        with pytest.raises(edge.HTTPException) as e:
            send(**bad)
        assert e.value.status_code == 404
    assert bridge == []


def test_bridge_commands_go_one_at_a_time_and_the_newest_row_wins(bridge, monkeypatch):
    monkeypatch.setattr(settings, "stack_command_gap_s", 0.2)
    times = []
    real = edge._get_json

    async def timed(client, url):
        times.append(asyncio.get_running_loop().time())
        return await real(client, url)

    monkeypatch.setattr(edge, "_get_json", timed)

    async def burst():
        rows = [dict(row=1, r=1), dict(row=2, r=2), dict(row=2, r=3), dict(row=2, r=4)]
        base = dict(stack=1, shelf=1, g=0, b=0, intensity=100)
        return await asyncio.gather(*(edge.stack_led(edge.StackLedIn(**(base | x))) for x in rows))

    out = asyncio.run(burst())
    sent = [u.split("?")[1].split("&r=")[1].split("&")[0] for u in bridge]
    assert sent == ["1", "4"], "row 1 once, then only the newest row-2 colour"
    assert [o.get("superseded", False) for o in out] == [False, True, True, False]
    assert times[1] - times[0] >= 0.19, "commands are spaced"


def test_off_is_black_for_that_row_only(bridge):
    send(intensity=0)
    assert bridge == ["http://10.0.0.9/led?path=0&start=104&end=190&r=0&g=0&b=0&brightness=255"]

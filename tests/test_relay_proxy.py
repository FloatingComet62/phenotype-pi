"""Relay proxy routing against a fake board. Run: python -m pytest tests/"""
import asyncio

import httpx
import pytest

from phenotype_edge import app as edge
from phenotype_edge.config import settings


class Board:
    """The relay firmware's contract: /relay, /ac, /status."""

    def __init__(self):
        self.state = {"relay1": False, "relay2": False, "relay3": False, "relay4": False, "ac": False}
        self.calls = []

    def handler(self, request: httpx.Request) -> httpx.Response:
        q = dict(request.url.params)
        self.calls.append(f"{request.url.path}?{request.url.query.decode()}".rstrip("?"))
        if request.url.path == "/relay":
            self.state[f"relay{q['ch']}"] = q["state"] == "on"
        elif request.url.path == "/ac":
            self.state["ac"] = q["state"] == "on"
        return httpx.Response(200, json=self.state)


@pytest.fixture
def board(monkeypatch):
    b = Board()
    monkeypatch.setattr(edge, "_client", lambda: httpx.AsyncClient(transport=httpx.MockTransport(b.handler)))
    monkeypatch.setattr(settings, "relay_host", "10.0.0.1")
    monkeypatch.setattr(settings, "ac2_host", "10.0.0.2")
    monkeypatch.setattr(settings, "valve_master_channel", "4")
    return b


def call(channel, state):
    return asyncio.run(edge.relay_proxy(edge.RelayProxyIn(channel=channel, state=state)))


def test_opening_a_valve_opens_the_master_first(board):
    out = call("valve2", True)
    assert out["ok"] and out["confirmed_state"] is True
    assert board.calls[:2] == ["/relay?ch=4&state=on", "/relay?ch=2&state=on"]
    assert board.state["relay4"] and board.state["relay2"]


def test_master_stays_open_while_another_branch_is_open(board):
    call("valve1", True)
    call("valve2", True)
    out = call("valve1", False)
    assert out["ok"] and out["confirmed_state"] is False
    assert board.state == {"relay1": False, "relay2": True, "relay3": False, "relay4": True, "ac": False}


def test_closing_the_last_branch_closes_the_master(board):
    call("valve3", True)
    call("valve3", False)
    assert not any(board.state[f"relay{n}"] for n in "1234")


def test_the_master_is_not_a_branch(board):
    with pytest.raises(edge.HTTPException) as e:
        call("valve4", True)
    assert e.value.status_code == 400 and board.calls == []


def test_ac_on_the_relay_board_is_inverted_and_ac2_is_not(board):
    assert call("ac", True) == {"ok": True, "confirmed_state": True, "device_response": board.state}
    assert board.state["ac"] is False          # output released = AC running
    board.state["ac"] = False
    out = call("ac2", True)
    assert out["ok"] and board.state["ac"] is True and board.calls[-2] == "/ac?state=on"


def test_raw_relay_channel_still_works(board):
    assert call("1", True)["ok"] and board.state["relay1"]

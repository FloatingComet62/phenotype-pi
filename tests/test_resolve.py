"""ARP selection for the board resolver. Run: python -m pytest tests/ (or python tests/test_resolve.py)"""
import asyncio
import importlib.util
import sys
import types

sys.modules.setdefault(
    "phenotype_edge.config",
    types.SimpleNamespace(settings=types.SimpleNamespace(board_mac_map={})),
)
spec = importlib.util.spec_from_file_location("phenotype_edge.resolve", "phenotype_edge/resolve.py")
resolve = importlib.util.module_from_spec(spec)
resolve.__package__ = "phenotype_edge"
spec.loader.exec_module(resolve)

TABLE = """192.168.8.195 dev wlan0 lladdr 38:3e:51:6f:45:a8 STALE
192.168.8.198 dev wlan0 lladdr 38:3e:51:6f:45:a8 REACHABLE
192.168.8.199 dev wlan0 lladdr 38:3e:51:6f:2b:e4 DELAY
192.168.8.193 dev wlan0 lladdr 38:3e:51:6f:2b:e4 FAILED
192.168.8.7 dev wlan0 INCOMPLETE"""


def test_arp_lookup_prefers_fresh_entry_and_ignores_failed():
    async def fake(*a, **k):
        return TABLE

    resolve._sh = fake
    assert asyncio.run(resolve._arp_lookup("38:3e:51:6f:45:a8")) == "192.168.8.198"
    assert asyncio.run(resolve._arp_lookup("38:3e:51:6f:2b:e4")) == "192.168.8.199"
    assert asyncio.run(resolve._arp_lookup("c0:cd:d6:d0:04:64")) is None


def test_a_remembered_address_that_moved_to_another_board_is_dropped():
    """14 Oct: dht4's old address went to dht2, and AC 2 commands went there."""
    dht4, dht2 = "38:3e:51:6f:2b:e4", "0c:b8:15:75:b4:70"
    resolve.settings = types.SimpleNamespace(board_mac_map={"dht4.local": dht4})
    resolve._cache["dht4.local"] = "192.168.8.198"
    neigh = {"192.168.8.198": f"192.168.8.198 dev wlan0 lladdr {dht2} REACHABLE"}

    async def fake_sh(*a, **k):
        return neigh.get(a[-1], "") if a[:4] == ("ip", "-4", "neigh", "show") and len(a) == 5 else ""

    async def fake_by_mac(mac):
        return "192.168.8.199" if mac == dht4 else None

    resolve._sh, resolve._by_mac = fake_sh, fake_by_mac
    assert asyncio.run(resolve.resolve("dht4.local")) == "192.168.8.199"
    neigh["192.168.8.199"] = f"192.168.8.199 dev wlan0 lladdr {dht4} REACHABLE"
    assert asyncio.run(resolve.resolve("dht4.local")) == "192.168.8.199", "still right: kept"


if __name__ == "__main__":
    test_a_remembered_address_that_moved_to_another_board_is_dropped()
    test_arp_lookup_prefers_fresh_entry_and_ignores_failed()
    print("ok")

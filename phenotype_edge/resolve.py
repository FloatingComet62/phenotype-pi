"""Board name -> IP: by MAC for known boards, Avahi browse for the rest.

The boards are DHCP clients, so their IPs move whenever the router
restarts. Their mDNS names are stable, but a unicast ``.local`` lookup
from this Pi takes 5-10 s or times out (the ESP32 responder answers
queries unreliably), which is longer than the poller waits. The boards do
announce ``_http._tcp`` on multicast though, and ``avahi-browse`` returns
that table in a few seconds, every time. So: browse, cache, fall back to
the last known address when a board is missing from a browse.

ponytail: one subprocess call per refresh; talk to avahi over D-Bus if it
ever needs to be faster.
"""

import asyncio
import ipaddress
import logging
import time

from .config import settings

logger = logging.getLogger("resolve")

_cache: dict[str, str] = {}   # name -> ip, resolved and in use
_browse: dict[str, str] = {}  # name -> ip, as Avahi last reported
_last_refresh = 0.0
_REFRESH_SECONDS = 60
_lock = asyncio.Lock()


async def refresh() -> None:
    global _last_refresh
    async with _lock:
        if time.monotonic() - _last_refresh < 5:
            return  # several callers hit a miss at once; one browse is enough
        try:
            proc = await asyncio.create_subprocess_exec(
                "avahi-browse", "-rtp", "_http._tcp",
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
            )
            out, _ = await asyncio.wait_for(proc.communicate(), timeout=20)
        except (OSError, asyncio.TimeoutError) as e:
            logger.warning("avahi-browse failed: %s", e)
            return
        found = {}
        for line in out.decode(errors="replace").splitlines():
            f = line.split(";")
            # "=;wlan0;IPv4;dht1;_http._tcp;local;dht1.local;192.168.8.198;80;"
            if len(f) > 7 and f[0] == "=" and f[2] == "IPv4" and f[7]:
                found[f[6].lower()] = f[7]
        _browse.clear()
        _browse.update(found)
        _last_refresh = time.monotonic()


async def _sh(*args, timeout=20) -> str:
    proc = await asyncio.create_subprocess_exec(
        *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL
    )
    out, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    return out.decode(errors="replace")


_STATE_RANK = {"REACHABLE": 0, "DELAY": 1, "PROBE": 1, "STALE": 2}


async def _arp_lookup(mac: str) -> str | None:
    """IP currently holding ``mac``. After leases move, the ARP table can
    hold the same MAC at an old and a new address; the freshest state wins."""
    best = None
    for line in (await _sh("ip", "-4", "neigh", "show")).splitlines():
        f = line.split()
        if "lladdr" not in f or f[f.index("lladdr") + 1].lower() != mac:
            continue
        rank = _STATE_RANK.get(f[-1])
        if rank is not None and (best is None or rank < best[0]):
            best = (rank, f[0])
    return best[1] if best else None


async def _sweep() -> None:
    """Ping every address on the Pi's /24 so the ARP table fills."""
    out = await _sh("ip", "-4", "-o", "addr", "show", "scope", "global")
    cidr = next((f[3] for f in (l.split() for l in out.splitlines()) if len(f) > 3), None)
    if not cidr:
        return
    net = ipaddress.ip_network(cidr, strict=False)
    hosts = list(net.hosts())[:254]

    async def ping(ip):
        proc = await asyncio.create_subprocess_exec(
            "ping", "-c", "1", "-W", "1", str(ip),
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
        )
        await proc.wait()

    for i in range(0, len(hosts), 64):
        await asyncio.gather(*(ping(ip) for ip in hosts[i : i + 64]))


_last_sweep = 0.0


async def _by_mac(mac: str) -> str | None:
    """Sweep (at most every 30 s) so stale entries age out, then look up."""
    global _last_sweep
    if time.monotonic() - _last_sweep > 30:
        _last_sweep = time.monotonic()
        await _sweep()
    return await _arp_lookup(mac)


async def resolve(host: str) -> str:
    """Return the IP for a ``.local`` name, or ``host`` itself otherwise.

    A board listed in BOARD_MACS is found by its MAC and nothing else: the
    MAC is the only identity that cannot be wrong. mDNS can be: on 28 Sep
    Avahi reported dht1 at dht4's address while the real dht1 announced
    itself as "dht1-2" after a name clash, and Zone 1 was fed Zone 4's
    readings. Avahi's browse table is used only for names with no MAC.

    A resolved address is kept until a request to it fails (see forget()),
    so the subnet sweep runs only when a board has actually gone missing.
    """
    if not host.endswith(".local"):
        return host
    name = host.lower()
    ip = _cache.get(name)
    if ip:
        return ip
    mac = settings.board_mac_map.get(name)
    if mac:
        ip = await _by_mac(mac)
        if ip:
            logger.info("%s is at %s (by MAC %s)", host, ip, mac)
    else:
        await refresh()
        ip = _browse.get(name)
    if ip is None:
        logger.warning("%s not found on the LAN", host)
        return host
    _cache[name] = ip
    return ip


def forget(host: str) -> None:
    """Call after a failed request so the next resolve re-browses."""
    global _last_refresh
    if host.endswith(".local"):
        _last_refresh = 0.0
        _cache.pop(host.lower(), None)

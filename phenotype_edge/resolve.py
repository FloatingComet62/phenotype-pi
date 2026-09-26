"""Board name -> IP, via Avahi's service browse.

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

_cache: dict[str, str] = {}
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
        moved = {n: ip for n, ip in found.items() if _cache.get(n) not in (None, ip)}
        if moved:
            logger.info("boards moved: %s", moved)
        _cache.update(found)
        _last_refresh = time.monotonic()


async def _sh(*args, timeout=20) -> str:
    proc = await asyncio.create_subprocess_exec(
        *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL
    )
    out, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    return out.decode(errors="replace")


async def _arp_lookup(mac: str) -> str | None:
    for line in (await _sh("ip", "-4", "neigh", "show")).splitlines():
        f = line.split()
        if "lladdr" in f and f[f.index("lladdr") + 1].lower() == mac and "FAILED" not in f:
            return f[0]
    return None


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


async def _by_mac(name: str) -> str | None:
    global _last_sweep
    mac = settings.board_mac_map.get(name)
    if not mac:
        return None
    ip = await _arp_lookup(mac)
    if ip is None and time.monotonic() - _last_sweep > 30:
        _last_sweep = time.monotonic()
        await _sweep()
        ip = await _arp_lookup(mac)
    return ip


async def resolve(host: str) -> str:
    """Return the IP for a ``.local`` name, or ``host`` itself otherwise.

    Order: Avahi browse cache, then the board's MAC in the ARP table (for a
    board whose mDNS responder has died), then the bare name.
    """
    if not host.endswith(".local"):
        return host
    name = host.lower()
    if name not in _cache or time.monotonic() - _last_refresh > _REFRESH_SECONDS:
        await refresh()
    ip = _cache.get(name)
    if ip is None:
        ip = await _by_mac(name)
        if ip:
            logger.info("%s found by MAC at %s (mDNS responder silent)", host, ip)
            _cache[name] = ip
    if ip is None:
        logger.warning("%s not seen on the LAN; trying the name directly", host)
        return host
    return ip


def forget(host: str) -> None:
    """Call after a failed request so the next resolve re-browses."""
    global _last_refresh
    if host.endswith(".local"):
        _last_refresh = 0.0
        _cache.pop(host.lower(), None)

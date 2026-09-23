# Phenotype Pi: edge service and sensor-board firmware

This repository holds everything that runs inside the grow room:

- **`phenotype_edge/`**: the Raspberry Pi edge service. One Uvicorn process on port **8090** polls the sensor boards, uploads camera stills, streams MJPEG, and proxies relay commands from the main backend to the boards.
- **`src/esp32_dht/`**: firmware for the ESP32 sensor boards (SHT20 temperature and humidity, optional solid-state-relay outputs). One firmware, one PlatformIO env per board.
- **`src/esp32_relay/`**: firmware for the 4-channel relay board plus AC line (`relay.local`).
- **`src/esp8266/`, `src/atmega328p/`**: older Uno WiFi R3 experiments, kept for reference.
- **`systemd/`**: the two units the Pi runs: the edge service and the reverse SSH tunnel to the VPS.

The main backend (FastAPI, `backend/` in the `phenotype-console` repo) and the dashboard (React, `phenotype-ui/` in the same repo) live elsewhere. This README covers the room side and the seam between the two.

## 1. How the whole thing fits together

~~~text
 grow-room Wi-Fi (192.168.8.0/24)                          VPS 103.25.130.37
 ┌──────────────────────────────────────────────┐          ┌──────────────────────────────┐
 │ dht1 .195  SHT20 + SSR ch1 = exhaust fan     │          │ Caddy (TLS)                  │
 │ dht2 .199  SHT20                             │  poll    │   └─ phenotype_api container │
 │ dht3 .198  SHT20                     ◄───────┼──────┐   │        FastAPI :8000         │
 │ dht4 .193  SHT20 + SSR (spare)               │      │   │        ▲ ingest (HTTPS)      │
 │ relay .200 4 relays + AC                     │      │   │        │ relay-proxy         │
 │                                              │      │   │        ▼ host.docker.internal│
 │ Raspberry Pi "pi"  ── phenotype-edge :8090 ──┼──────┴───┼──────► :18090 (reverse SSH)  │
 │                    ── phenotype-tunnel (ssh -R) ────────►│                              │
 └──────────────────────────────────────────────┘          └──────────────────────────────┘
                                                                        ▲
                                                     dashboard (Vercel) ┘  wss /ws/live
~~~

Data flows one way and commands flow the other:

1. **Readings (room → cloud).** Every 60 s the edge service GETs `/reading` on each board in `DHT_HOSTS`, in order. Position *n* in that list is **Zone n**. It resolves the sensor ids by the exact names `Zone n Temperature` and `Zone n Humidity` from the backend's `GET /sensors`, then POSTs a batch to `/ingest/sensor-readings/batch` with the `X-API-Key` header. The backend fans each reading out over its websocket, which is how the dashboard updates live.
2. **Camera (room → cloud).** Five stills a day from the Pi camera go to `/ingest/camera-capture`. The backend runs its CV pipeline on each one.
3. **Commands (cloud → room).** A toggle on the dashboard becomes `POST /v2/actuators/{id}/toggle` on the backend. If that actuator has a relay binding, the backend POSTs `{"channel": ..., "state": ...}` to the Pi's `/relay-proxy`. The Pi cannot be reached from the internet, so it keeps a reverse SSH tunnel open to the VPS; the backend container sees the Pi's port 8090 as `host.docker.internal:18090`. The Pi then GETs the right board, re-reads its `/status`, and answers with the confirmed state. The backend only flips its own state on a confirmed answer, and shows the error inline otherwise.

Nothing in the room talks to the internet except the Pi. The boards are plain HTTP servers on the LAN with no authentication; the Pi is the only intended client.

## 2. What is deployed right now (23 Sep 2026)

| Board | Hostname | IP | MAC | Firmware env | Notes |
| --- | --- | --- | --- | --- | --- |
| Zone 1 | dht1 | 192.168.8.195 | 38:3e:51:6f:45:a8 | `esp32_dht` | SSR ch1 (GPIO 25) drives the exhaust fan. Drops off Wi-Fi for minutes at a time; see §8. |
| Zone 2 | dht2 | 192.168.8.199 | 0c:b8:15:75:b4:70 | `esp32_dht2` | no SSR |
| Zone 3 | dht3 | 192.168.8.198 | 08:a6:f7:b1:39:48 | `esp32_dht3` | no SSR |
| Zone 4 | dht4 | 192.168.8.193 | 38:3e:51:6f:2b:e4 | `esp32_dht4` | SSR code present, nothing wired |
| relay | relay | 192.168.8.200 | | `esp32_relay` (older build) | Lights 1-4 + AC |

On the Pi (`phenotype@pi`, reachable as `ssh pi.apsdev.in` through the Cloudflare tunnel):

- edge service checkout: `~/phenotype-pi-new` (this repo), venv built with uv, unit `phenotype-edge.service`
- tunnel: `phenotype-tunnel.service`, key `~/.ssh/id_ed25519_tunnel`
- `DHT_HOSTS` lists the four boards **by IP**, because the Pi resolves the boards' mDNS names only intermittently while HTTP by IP is reliable
- `/etc/hosts` pins the backend hostname to its IP, because the room router's DNS takes 5-15 s per lookup and the poller gives up at 5 s

On the VPS (`ssh -p 5726 i2k2-admin@103.25.130.37`): `/opt/phenotype` holds a plain copy of the backend, built with `docker compose up -d --build api`. `/etc/ssh/sshd_config.d/phenotype-tunnel.conf` sets `GatewayPorts clientspecified`, and ufw allows Docker subnets to reach `172.17.0.1:18090`.

Passwords and the ingest API key are not in this repository. Ask the owner.

## 3. Sensor-board firmware (`src/esp32_dht`)

One sketch serves every ESP32 sensor board. Per-board differences are build flags in `platformio.ini`:

~~~ini
[env:esp32_dht]        ; dht1, with SSR outputs
[env:esp32_dht2]       ; extends esp32_dht, -DHOSTNAME_STR='"dht2"' -DNO_SSR
[env:esp32_dht3]       ; ... "dht3", -DNO_SSR
[env:esp32_dht4]       ; ... "dht4"
~~~

Adding a board is three lines: copy an env block, change the name, decide whether it needs `-DNO_SSR`.

What the firmware does:

- **SHT20 over I2C.** At boot it probes for the sensor at 0x40 on SDA/SCL **18/19** first, then **21/22**, and prints which pair answered. Wire new boards to 18/19. Readings are taken every 1.5 s into a cache; `/reading` never blocks on the sensor.
- **Wi-Fi keepalive.** Auto-reconnect is on, and every 15 s the loop checks the link. When the link comes back it restarts the mDNS responder, because the ESP32 responder silently dies across a reconnect and the board would otherwise keep its IP but stop answering to its name.
- **SSR outputs** (unless `NO_SSR`): GPIO **25** is channel 1, GPIO **26** is channel 2, driven HIGH for on. Both are set low before the pins are enabled so nothing flicks on at boot. A power cycle therefore always turns the fan off.

Endpoints:

| Route | Response |
| --- | --- |
| `GET /reading` | `{"temperature_c":34.4,"humidity_percent":39.5,"age_seconds":0,"fail_count":0,"success_count":1530}` or 503 `{"error":"No valid reading yet"}` |
| `GET /relay?ch=1&state=on` | `{"relay1":true,"relay2":false}` (SSR builds only, ch 1-2, state on/off) |
| `GET /status` | `{"relay1":false,"relay2":false}` (SSR builds only) |

### Wiring the SSR

For a single-channel DC-controlled SSR such as the Fotek SSR-40 DA in `references/ssr-40da-fotek.jpeg`, only the DC input side goes to the ESP32:

| SSR terminal | ESP32 |
| --- | --- |
| 3 (+, 3-32 V DC) | GPIO 25 (channel 1) or GPIO 26 (channel 2) |
| 4 (−) | GND |

Terminals 1 and 2 go in series with the appliance's live wire. Neutral bypasses the SSR. A 3.3 V drive is at the bottom of the input range; if the SSR's LED lights but the load does not switch, add an NPN transistor and feed the SSR's + from 5 V. Anything above about 10 A needs a heatsink whatever the label says. Two-channel SSR *modules* with a VCC pin usually want 5 V on VCC and are active-low; for those flip `SSR_ON`/`SSR_OFF` in the sketch.

### Flashing

Prerequisites: VS Code with the PlatformIO IDE extension (recommended in `.vscode/extensions.json`), and `src/secrets.h` copied from `src/secrets.h.example` with the room's Wi-Fi credentials. That file is git-ignored.

1. Plug the board in. On Linux it appears as `/dev/ttyUSB0` (CP2102); on Windows as a COM port. Change `upload_port`/`monitor_port` in `platformio.ini` if yours differs.
2. **Check which board you have before writing.** Boards look identical; the MAC does not lie:
   ~~~bash
   ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/ttyUSB0 read_mac
   ~~~
   Compare with the table in §2.
3. In VS Code, pick the env in the status bar and click Upload (Ctrl+Alt+U), or from a terminal:
   ~~~bash
   ~/.platformio/penv/bin/pio run -e esp32_dht3 -t upload
   ~~~
4. Watch the boot banner with the serial monitor (Ctrl+Alt+S). A healthy board prints the SHT20 pin pair, its IP, and `mDNS responder started: http://dht3.local`.

Two things that bit us:

- The CP2102 on these boards drops out at the default 460800 baud. `upload_speed = 115200` is set on the env for that reason.
- **An interrupted upload leaves the board reboot-looping** (`rst:0x3 (SW_RESET)` three times a second, nothing after `entry 0x...`). It looks like a wiring fault. It is not. Flash again.

### The relay board (`src/esp32_relay`)

Same structure as the SSR half of the sensor firmware, with four channels (`/relay?ch=1..4`), `/all`, `/ac`, and `/status`. The AC output is wired inverted on the physical board and the Pi compensates. The deployed board still runs an older build with hostname `relay`; this sketch defaults to `esp32-relay`, so keep `RELAY_HOST` on the Pi in step with whatever is flashed.

## 4. Edge service: setup on the Pi

Prerequisites: Raspberry Pi OS with Python 3.13.5 available to [uv](https://docs.astral.sh/uv/) (pinned in `.python-version` and `pyproject.toml`), plus the OS packages for the camera:

~~~bash
sudo apt update
sudo apt install -y python3-picamera2 python3-opencv
~~~

Install:

~~~bash
cd ~/phenotype-pi-new
uv venv --python 3.13.5 --system-site-packages
source .venv/bin/activate
uv sync --active --locked
cp .env.example .env     # then fill in MAIN_BACKEND_URL, INGEST_API_KEY, DHT_HOSTS, EXHAUST_FAN_HOST
~~~

`--system-site-packages` is required so the venv sees the apt-installed Picamera2 and OpenCV. If an import fails with `source code string cannot contain null bytes`, a package file was truncated by a power cut: recreate with `uv venv --clear ...` and `uv sync --active --locked --reinstall --no-cache`.

Run by hand:

~~~bash
uv run --no-sync uvicorn phenotype_edge.app:app --host 0.0.0.0 --port 8090
~~~

The committed unit assumes `/home/pi/phenotype` and user `pi`. The live Pi uses `/home/phenotype/phenotype-pi-new` and user `phenotype`; edit `WorkingDirectory`, `ExecStart` and `User` together before installing.

## 5. Configuration (`.env`)

| Variable | Default | Purpose |
| --- | --- | --- |
| MAIN_BACKEND_URL | deployment-specific | Backend API base, e.g. `https://phenotype.103-25-130-37.nip.io/api/v1`. |
| INGEST_API_KEY | deployment-specific | `X-API-Key` for the two ingest routes. Must match the backend's. |
| DHT_HOSTS | dht1.local,dht2.local,dht3.local | Boards to poll, in zone order. **Use IPs** until the router has DHCP reservations and the Pi resolves `.local` names reliably. |
| DHT_POLL_INTERVAL_SECONDS | 60 | |
| DHT_READING_PATH | /reading | |
| DHT_TEMP_FIELD / DHT_HUMIDITY_FIELD | temperature_c / humidity_percent | Preferred JSON keys; `temp_c`, `temperature`, `humidity`, `humidity_pct` are accepted too. |
| CAMERA_ENABLED, CAMERA_ID, CAMERA_BACKEND, CAMERA_DEVICE_INDEX, CAPTURE_INTERVAL_SECONDS | true, 1, picamera2, 0, 17280 | Periodic stills. 8640 s = 10/day is what runs today. |
| RELAY_PROXY_ENABLED | true | Enables `POST /relay-proxy`. |
| RELAY_HOST | relay.local | The 4-channel relay board. |
| EXHAUST_FAN_HOST | *(empty)* | Board whose SSR drives the exhaust fan (`192.168.8.195`). Empty means channel `exhaust` returns 502. |
| EXHAUST_FAN_CHANNEL | 1 | SSR channel on that board. |
| REQUEST_TIMEOUT_SECONDS | 8 | For board, relay and ingest requests. |

Sensor names in the backend must be `Zone {n} Temperature` and `Zone {n} Humidity`. The backend provisions that pair automatically for every zone in the farm layout, so adding a zone on the dashboard's Farm setup page plus a board in `DHT_HOSTS` is all that is needed for a new zone.

## 6. Endpoints

### GET /health

~~~json
{"status":"ok","relay_proxy_enabled":true,"camera_enabled":true}
~~~

### GET /video

MJPEG, `multipart/x-mixed-replace; boundary=frame`, each part `--frame\r\n`, `Content-Type: image/jpeg`, JPEG bytes, `\r\n`. Canonical URL `http://<pi>:8090/video`. The old port-5000 streamer is gone.

### POST /relay-proxy

~~~json
{"channel":"1","state":true}
~~~

`channel` is one of:

| channel | goes to |
| --- | --- |
| `1`..`4` | `RELAY_HOST` `/relay?ch=N` |
| `ac` | `RELAY_HOST` `/ac`, inverted on the wire and in the returned state to match the physical wiring |
| `exhaust` | `EXHAUST_FAN_HOST` `/relay?ch=EXHAUST_FAN_CHANNEL` |

The service sends the action, then GETs the same board's `/status` and returns `{"ok": <confirmed == requested>, "confirmed_state": ..., "device_response": {...}}`. 403 when disabled, 502 when the board is unreachable or answers something unexpected.

### Camera coordination

One lock-protected Picamera2 owner serves both the stream and the periodic stills. A still switches to the still configuration, captures, uploads as multipart field `file`, and restores the video configuration. An open stream pauses for one capture and resumes. `CAMERA_DEVICE_INDEX` only affects the USB still backend.

## 7. systemd on the Pi

Two units:

~~~bash
sudo install -m 0644 systemd/phenotype-edge.service   /etc/systemd/system/
sudo install -m 0644 systemd/phenotype-tunnel.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now phenotype-edge phenotype-tunnel
journalctl -u phenotype-edge -f
~~~

`phenotype-tunnel.service` runs:

~~~text
ssh -NT -R 172.17.0.1:18090:127.0.0.1:8090 -p 5726 i2k2-admin@103.25.130.37
~~~

with keepalives, `ExitOnForwardFailure`, `Restart=always`. On the VPS the Pi's key is authorised with `restrict,port-forwarding,permitlisten="172.17.0.1:18090"`, so that key can open exactly this listener and nothing else: no shell, no other ports. The listener is on the Docker bridge address only, never on the public interface.

To set this up on a fresh Pi: `ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519_tunnel`, append the public key to the VPS user's `authorized_keys` with the options above, make sure the VPS sshd has `GatewayPorts clientspecified` and ufw allows `172.16.0.0/12` to `172.17.0.1 port 18090`, then enable the unit.

## 8. Troubleshooting

~~~bash
# Edge service alive, and what it thinks of the boards
curl -s http://127.0.0.1:8090/health
journalctl -u phenotype-edge --since -5min | grep dht_poller

# A board, by IP (name resolution is the unreliable part)
curl -s http://192.168.8.195/reading
curl -s http://192.168.8.195/status

# Who is on the LAN right now
avahi-browse -rtp _http._tcp | grep '^=' | cut -d';' -f4,8
ip neigh | grep -i '38:3e:51\|0c:b8:15\|08:a6:f7'

# Tunnel
systemctl status phenotype-tunnel
# and on the VPS:
ss -ltn | grep 18090
docker exec phenotype_api python -c "import urllib.request;print(urllib.request.urlopen('http://host.docker.internal:18090/health').read())"
~~~

Symptoms we have seen and what they meant:

- **Dashboard toggle says "Edge service at http://host.docker.internal:18090 timed out".** Either the tunnel is down (check the Pi unit) or the board behind that channel is off the network (curl it by IP from the Pi). The backend keeps the old state and shows the error inline.
- **A zone goes stale while its board answers `curl` by IP.** Name resolution. Put the IP in `DHT_HOSTS`.
- **`Failed to read .../reading: 503`.** The board is up but its SHT20 is not answering: wrong pins or a dead sensor. The serial banner says which pin pair it found, if any.
- **Board unreachable for minutes, then back.** dht1 does this. Suspect its supply or its position relative to the access point; the other three boards on the same firmware do not drop.
- **All zones offline, `phenotype-edge` in a restart loop with `203/EXEC`.** The unit points at a path that no longer exists. Fix `ExecStart`.
- **`Couldn't reach main backend to resolve sensor IDs`.** DNS on the Pi is slow. The backend hostname is pinned in `/etc/hosts`; if the backend moves, update that line.

## 9. How we proceed

In rough priority order:

1. **DHCP reservations on the room router** for the four sensor boards and the relay board, so the IP list in `DHT_HOSTS` and `EXHAUST_FAN_HOST` can never go stale. Until then, if a board moves, the zone shows stale and the fix is one line in `.env` plus a restart.
2. **Make dht1 stay up.** It drives the fan, so it matters most. Check its power supply and Wi-Fi signal; if it still drops, move the SSR to dht4 (already has the firmware, spare channel wired to GPIO 25) and point `EXHAUST_FAN_HOST` there.
3. **A proper hostname for the backend** (an A record such as `phenotype-api.<yourdomain>` → 103.25.130.37) instead of nip.io, which some networks block by SNI. Update Caddy, the Pi's `.env` and `/etc/hosts`, and the dashboard's `VITE_API_BASE`.
4. **Reflash the relay board** with `src/esp32_relay` so it gets the same reconnect logic as the sensor boards. Decide on the hostname (`relay` vs `esp32-relay`) and set `RELAY_HOST` to match.
5. **Wire the rest of the farm actuators.** AC 1, AC 2, circulation pumps, water valves and row LEDs exist in the backend as logical devices. Each needs a physical output and a relay-proxy channel. The pattern is the one used for the fan: a named channel in `/relay-proxy` mapped to a board and a channel via `.env`, and a `FARM_ACTUATOR_CHANNELS` entry in the backend's `provision.py`. When a second board of SSRs appears, replace `EXHAUST_FAN_HOST` with a small channel→host map.
6. **Per-stack sensors.** The backend already has slots for two temperature and two humidity sensors, pH and water level per stack. The same ESP32 firmware can serve them once the boards exist; the poller will need a name-mapping beyond `Zone n`.
7. **Camera 2** has not uploaded since early September; its Pi (pi2) needs the same edge setup.
8. **Cell → row mapping** on the dashboard is deferred until the camera-to-shelf layout is decided.
9. **Authentication on the boards** is absent. Anyone on the room Wi-Fi can switch the fan. A shared secret in the query string, checked by the firmware and sent by the Pi, is a small change once the Wi-Fi stops being trusted.
10. **Rotate the room Wi-Fi password.** It was committed in `src/esp8266/main.cpp` in this repository's history before credentials moved to the git-ignored `src/secrets.h`.

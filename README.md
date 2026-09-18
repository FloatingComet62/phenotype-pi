# Phenotyping Pi edge service

One Uvicorn process serves the edge API, MJPEG camera stream, DHT poller, and optional periodic camera upload. It listens on port **8090**.

~~~text
Main backend --Tailscale--> this Pi --mDNS--> DHT nodes and relay
                                      └--> GET /video
~~~

## Prerequisites

- Raspberry Pi OS with Python 3.13.5 available to uv. The repository pins this in both **.python-version** and **pyproject.toml**.
- [uv](https://docs.astral.sh/uv/).
- For an official Pi Camera Module and MJPEG streaming: **python3-picamera2** and OpenCV from Raspberry Pi OS.

~~~bash
sudo apt update
sudo apt install -y python3-picamera2 python3-opencv
~~~

For a USB still camera, add OpenCV to the locked project before deployment:

~~~bash
uv add opencv-python-headless
~~~

## Setup

The deployment root is **/home/pi/phenotype**; the committed systemd unit uses that path and the **pi** user. If you deploy elsewhere, update both **WorkingDirectory** and **ExecStart** together.

~~~bash
cd /home/pi/phenotype
uv venv --python 3.13.5 --system-site-packages
source .venv/bin/activate
uv sync --active --locked
cp .env.example .env
~~~

**--system-site-packages** is required for the apt-installed Picamera2 and Raspberry Pi OpenCV bindings. Runtime dependencies are declared in **pyproject.toml** and pinned in **uv.lock**. **requirements.txt** remains only as a temporary camera-compatibility fallback until this exact uv workflow is exercised on a deployment Pi. Do not use it to install the uv environment.

Edit **.env** without committing it. The ignore rules cover **.env**, its backups, and local virtual environments.

## Configuration

| Variable | Default | Purpose |
| --- | --- | --- |
| MAIN_BACKEND_URL | deployment-specific | Main backend API base URL. |
| INGEST_API_KEY | deployment-specific | X-API-Key sent with DHT and camera ingestion. |
| DHT_HOSTS | dht1.local,dht2.local,dht3.local | Ordered DHT hosts; order determines zone numbering. |
| DHT_POLL_INTERVAL_SECONDS | 60 | Delay between DHT polling cycles. |
| DHT_READING_PATH | /reading | Reading endpoint on every DHT host. |
| DHT_TEMP_FIELD | temperature_c | Preferred temperature field; aliases remain accepted. |
| DHT_HUMIDITY_FIELD | humidity_percent | Preferred humidity field; aliases remain accepted. |
| CAMERA_ENABLED | true in code | Enables periodic still capture and upload. |
| CAMERA_ID | 1 | Existing backend camera identifier. |
| CAMERA_BACKEND | picamera2 | picamera2, usb, or none. |
| CAMERA_DEVICE_INDEX | 0 | USB camera index only. |
| CAPTURE_INTERVAL_SECONDS | 900 in code | Periodic still-upload interval. |
| RELAY_PROXY_ENABLED | true | Enables POST /relay-proxy. |
| RELAY_HOST | relay.local | Relay mDNS hostname. The current effective default is deliberately **not** changed to esp32-relay.local. |
| LISTEN_HOST | 0.0.0.0 | Retained settings value; the Uvicorn command/unit controls the actual listener. |
| LISTEN_PORT | 8090 | Retained settings value; the Uvicorn command/unit controls the actual listener. |
| REQUEST_TIMEOUT_SECONDS | 8 | Timeout for DHT, relay, and ingestion requests. |

DHT sensor names in the backend must be **Zone {n} Temperature** and **Zone {n} Humidity**, where **{n}** is the one-based position in **DHT_HOSTS**. The poller accepts temperature_c, temp_c, or temperature, and humidity_percent, humidity, or humidity_pct, while preferring the configured names.

## Run

~~~bash
cd /home/pi/phenotype
uv run --no-sync uvicorn phenotype_edge.app:app --host 0.0.0.0 --port 8090
~~~

No camera hardware is opened by importing **phenotype_edge.app**; the Pi camera is opened lazily when a stream or Pi-camera capture needs it.

## Endpoints

### GET /health

Returns:

~~~json
{"status":"ok","relay_proxy_enabled":true,"camera_enabled":true}
~~~

### GET /video

Streams MJPEG with this exact media type:

~~~text
multipart/x-mixed-replace; boundary=frame
~~~

Each part is **--frame\r\n**, **Content-Type: image/jpeg**, a JPEG encoded from the Pi camera’s RGB-to-BGR OpenCV pipeline, then **\r\n**. The autofocus controls remain **{"AfMode": 1, "AfTrigger": 0}**.

The canonical stream URL is **http://&lt;pi-address&gt;:8090/video**. Any external client still using **http://&lt;pi-address&gt;:5000/video** must be updated to this URL; no legacy port-5000 listener remains. No such caller exists in this checkout.

### POST /relay-proxy

Request body:

~~~json
{"channel":"1","state":true}
~~~

**channel** accepts a numbered relay channel or **ac**. The service sends the relay action request and then performs a separate GET /status confirmation. It returns 403 when disabled and 502 for an unreachable, malformed, or incomplete relay response. The **ac** relay is intentionally inverted in both the action request and returned logical state.

## Camera coordination

The old deployment used separate processes for live video and periodic Pi-camera stills. The unified service has one lock-protected Picamera2 owner. A periodic still capture temporarily switches to the still configuration, captures JPEG data with the existing upload field name (**file**) and endpoint, then restores the video configuration before another MJPEG frame is captured. Frame acquisition and still capture are serialized; an active stream can pause for one still capture but is not closed.

The live stream retains the original Pi-camera pipeline. **CAMERA_DEVICE_INDEX** applies only to the periodic USB still-capture backend; it does not redirect the legacy Pi-camera stream to a guessed USB device.

## systemd

~~~bash
sudo cp /home/pi/phenotype/systemd/phenotype-edge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now phenotype-edge
sudo systemctl status phenotype-edge
journalctl -u phenotype-edge -f
~~~

The unit starts exactly one listener: **uvicorn phenotype_edge.app:app --host 0.0.0.0 --port 8090**.

## Troubleshooting

~~~bash
# Verify the local edge API and stream headers
curl -i http://127.0.0.1:8090/health
curl -i --max-time 5 http://127.0.0.1:8090/video

# Verify mDNS reachability from the Pi
curl http://relay.local/status
curl http://dht1.local/reading

# Follow polling, camera, and relay failures
journalctl -u phenotype-edge -f
~~~

- A missing backend sensor name drops only that reading; register the matching Zone {n} sensor and the poller will resolve it again.
- If the backend resets its sensor database, a dropped_sensor_ids response causes an immediate sensor-ID refresh; a periodic refresh is also performed.
- If Picamera2 import or camera initialization fails, periodic capture disables itself for that process run. Verify the apt packages, camera hardware, and **--system-site-packages**.
- A real Pi must validate camera coexistence: keep **/video** open through one scheduled capture and confirm the stream resumes with changing frames and the backend receives the JPEG upload.

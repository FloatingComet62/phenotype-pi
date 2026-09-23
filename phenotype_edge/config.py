from pydantic_settings import BaseSettings


class Settings(BaseSettings):
    # --- Main backend (on psi, reached over Tailscale) ---
    main_backend_url: str = "http://100.75.13.45:8000/api/v1"
    ingest_api_key: str = (
        "myapikeyforeg"  # must match the main backend's INGEST_API_KEY
    )

    # --- DHT sensor polling ---
    # Comma-separated mDNS hostnames, each expected to respond to a GET
    # at dht_reading_path with a JSON body containing temperature +
    # humidity fields.
    dht_hosts: str = "dht1.local,dht2.local,dht3.local"
    dht_poll_interval_seconds: float = 60.0
    dht_reading_path: str = "/reading"  # e.g. GET http://dht1.local/reading
    # Field names in each device's JSON response -- adjust to match your
    # actual DHT firmware if it differs.
    dht_temp_field: str = "temperature_c"
    dht_humidity_field: str = "humidity_percent"
    # Sensors must already exist in the main backend (see
    # register_hardware.py / seed_demo_data.py) -- this poller looks
    # them up by name at startup using the pattern "Zone {n} Temperature"
    # / "... Humidity", where zone n = position in DHT_HOSTS (1-indexed) --
    # not the hostname itself.

    # --- Camera capture (periodic stills, not live video) ---
    camera_enabled: bool = True
    camera_id: int = 1  # must match a camera already registered in the main backend
    camera_backend: str = (
        "picamera2"  # "picamera2" (Pi Camera Module) | "usb" (OpenCV) | "none"
    )
    camera_device_index: int = 0  # only used when camera_backend=usb
    # capture_interval_seconds: float = 17280.0  # 5x/day (24h / 5) -- matches the CV pipeline's intended cadence
    capture_interval_seconds: float = 15 * 60

    # --- Relay proxy (lets the main backend toggle lights/AC via the relay) ---
    relay_proxy_enabled: bool = True
    # Must match HOSTNAME in esp32_relay.ino -- the firmware as shipped
    # uses "esp32-relay" (-> esp32-relay.local), not "relay".
    relay_host: str = "relay.local"
    # The exhaust fan hangs off an SSR on the Zone 1 sensor board (esp32_dht
    # firmware, GET /relay?ch=N), not the 4-channel relay. Channel "exhaust"
    # on /relay-proxy goes there instead. Empty host = not wired.
    # ponytail: one named channel; a host map if a second SSR appears.
    exhaust_fan_host: str = ""
    exhaust_fan_channel: str = "1"
    listen_host: str = "0.0.0.0"
    listen_port: int = 8090

    request_timeout_seconds: float = 8.0

    @property
    def dht_host_list(self) -> list[str]:
        return [h.strip() for h in self.dht_hosts.split(",") if h.strip()]

    class Config:
        env_file = ".env"


settings = Settings()

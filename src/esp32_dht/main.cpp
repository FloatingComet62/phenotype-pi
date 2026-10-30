#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Wire.h>
#include "../secrets.h"
#include <Preferences.h>
#include <esp_task_wdt.h>

// ---------- Config ----------
// The farm's network only (WIFI_SSID_FARM in secrets.h, the Poly House
// router's 2.4 GHz band). ponytail: WiFiMulti with one entry; add more addAP
// lines if a board ever has to roam between routers.
WiFiMulti wifiMulti;

void addNetworks() {
  wifiMulti.addAP(WIFI_SSID_FARM, WIFI_PASSWORD_FARM);
}
#ifndef HOSTNAME_STR
#define HOSTNAME_STR "dht1"   // override per board with build_flags in platformio.ini
#endif
const char* HOSTNAME = HOSTNAME_STR;

#define SHT20_ADDR 0x40
#define TEMP_CMD 0xF3    // Hold master, read temperature
#define HUM_CMD 0xF5     // Hold master, read humidity

#ifndef NO_SSR
// Two SSR channels. Most SSRs trigger on a HIGH input (3-32 V DC); flip SSR_ON
// to LOW if yours is active-low. Pins 25/26 are free of strapping and I2C duty.
const int SSR_PINS[2] = {25, 26};
#define SSR_ON  HIGH
#define SSR_OFF LOW
bool ssrState[2] = {false, false};
Preferences prefs;  // SSR state survives a reboot (see watchdog below)
#endif

WebServer server(80);

// ---------- Cached sensor state ----------
float lastTemp = NAN;
float lastHum = NAN;
unsigned long lastReadTime = 0;
unsigned long lastGoodReadTime = 0;
const unsigned long READ_INTERVAL = 1500;
int failCount = 0;
int successCount = 0;

// ---------- SHT20 I2C read functions ----------
float readSHT20(uint8_t command) {
  Wire.beginTransmission(SHT20_ADDR);
  Wire.write(command);
  if (Wire.endTransmission() != 0) return NAN;  // I2C error
  
  delay(85);  // Measurement time for SHT20
  
  if (Wire.requestFrom(SHT20_ADDR, 3) != 3) return NAN;  // Should read 2 data + 1 checksum
  
  uint16_t raw = (Wire.read() << 8) | Wire.read();
  uint8_t checksum = Wire.read();
  
  // Optional: verify checksum (more robust)
  // if (!verifySHT20Checksum(raw, checksum)) return NAN;
  
  raw &= 0xFFFC;  // Clear status bits
  
  if (command == TEMP_CMD) {
    return -46.85 + 175.72 * (raw / 65536.0);  // Temperature formula
  } else {
    return -6.0 + 125.0 * (raw / 65536.0);     // Humidity formula
  }
}

// ---------- I2C ----------
// Boards have been wired to either SDA/SCL pair; probe for the SHT20 (0x40)
// on each and keep the first that ACKs. Falls back to 18/19.
int sdaPin = 18, sclPin = 19;

bool probeI2c() {
  const int I2C_PAIRS[][2] = {{18, 19}, {21, 22}};
  for (auto& pr : I2C_PAIRS) {
    Wire.end();
    Wire.begin(pr[0], pr[1]);
    Wire.setClock(100000);  // SHT20 max is 400kHz, but 100kHz is safer
    Wire.setTimeOut(50);    // a missing or stuck sensor must not stall loop()
    Wire.beginTransmission(SHT20_ADDR);
    if (Wire.endTransmission() == 0) {
      sdaPin = pr[0]; sclPin = pr[1];
      Serial.printf("SHT20 found on SDA=%d SCL=%d\n", sdaPin, sclPin);
      return true;
    }
    Serial.printf("no SHT20 on SDA=%d SCL=%d\n", pr[0], pr[1]);
  }
  Wire.end();
  Wire.begin(18, 19);
  Wire.setClock(100000);
  Wire.setTimeOut(50);
  sdaPin = 18; sclPin = 19;
  Serial.println("SHT20 not found on either pair; check wiring");
  return false;
}

// A sensor that glitched (loose wire, noise from the SSR) can leave SDA held
// low, and every later read fails until a reboot. Nine SCL pulses release
// it; then the pins are probed again, which also finds a sensor that was
// plugged in after boot.
int consecutiveFails = 0;
unsigned long i2cRecoveries = 0;

void recoverI2c() {
  Wire.end();
  pinMode(sdaPin, INPUT_PULLUP);
  pinMode(sclPin, OUTPUT);
  for (int i = 0; i < 9; i++) {
    digitalWrite(sclPin, LOW);  delayMicroseconds(5);
    digitalWrite(sclPin, HIGH); delayMicroseconds(5);
  }
  i2cRecoveries++;
  probeI2c();
}

// ---------- Background sensor update ----------
void updateSensor() {
  unsigned long now = millis();
  if (now - lastReadTime < READ_INTERVAL) return;
  lastReadTime = now;

  float t = readSHT20(TEMP_CMD);
  float h = readSHT20(HUM_CMD);

  if (isnan(t) || isnan(h) || t < -40 || t > 125 || h < 0 || h > 100) {
    failCount++;
    if (++consecutiveFails >= 5) {   // ~7.5 s of failures
      consecutiveFails = 0;
      recoverI2c();
    }
    return;
  }
  consecutiveFails = 0;

  lastTemp = t;
  lastHum = h;
  lastGoodReadTime = now;
  successCount++;
}

// ---------- No-request watchdog ----------
// The Pi polls /reading every 60 s. A board that has served nothing for
// 5 min while believing it is online is wedged (we have seen boards answer
// ARP but not TCP for many minutes); a reboot is the only remote fix.
// Outputs are restored from flash at boot so the reboot is invisible.
unsigned long lastServed = 0;
const unsigned long SERVE_TIMEOUT = 5UL * 60UL * 1000UL;

void touchServed() { lastServed = millis(); }

// Free memory is checked too: this sketch allocates almost nothing, so a
// heap below 20 KB means something leaks, and a clean restart beats running
// on until the network stack cannot get a buffer.
const uint32_t MIN_FREE_HEAP = 20000;

void checkWatchdog() {
  if (millis() - lastServed > SERVE_TIMEOUT) {
    Serial.println("no HTTP request served in 5 min, rebooting");
    delay(100);
    ESP.restart();
  }
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    Serial.printf("free heap %u B, rebooting\n", ESP.getFreeHeap());
    delay(100);
    ESP.restart();
  }
}

// Hardware watchdog: if loop() itself hangs (Wi-Fi driver, I2C), the checks
// above never run. This resets the chip after 30 s without a loop pass.
const int HARDWARE_WATCHDOG_S = 30;

void startHardwareWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t config = {.timeout_ms = HARDWARE_WATCHDOG_S * 1000,
                                  .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&config);
#else
  esp_task_wdt_init(HARDWARE_WATCHDOG_S, true);
#endif
  esp_task_wdt_add(NULL);
}

// ---------- Wi-Fi keepalive ----------
// The ESP32 core auto-reconnects the link, but the mDNS responder stays dead
// after a drop, so the board keeps its IP yet stops answering to dht1.local.
bool wifiUp = false;
unsigned long lastWifiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 15000;

void startMdns() {
  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS responder started: http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("Error starting mDNS");
  }
}

void ensureWifi() {
  unsigned long now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = now;

  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiUp) {
    Serial.print("WiFi up, IP: ");
    Serial.println(WiFi.localIP());
    startMdns();
  } else if (!up) {
    Serial.println("WiFi down, reconnecting");
    wifiMulti.run(8000);   // any known network; well inside the 30 s watchdog
  }
  wifiUp = up;
}

#ifndef NO_SSR
// ---------- SSR ----------
void setSsr(int idx, bool on) {
  if (idx < 0 || idx > 1) return;
  ssrState[idx] = on;
  digitalWrite(SSR_PINS[idx], on ? SSR_ON : SSR_OFF);
  prefs.putUChar("ssr", (ssrState[0] ? 1 : 0) | (ssrState[1] ? 2 : 0));
}

String ssrJson() {
  return "{\"relay1\":" + String(ssrState[0] ? "true" : "false") +
         ",\"relay2\":" + String(ssrState[1] ? "true" : "false") + "}";
}

// GET /relay?ch=1|2&state=on|off  (same shape as the esp32_relay board)
void handleRelay() {
  touchServed();
  if (!server.hasArg("ch") || !server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"missing ch or state param\"}");
    return;
  }
  int ch = server.arg("ch").toInt();
  String state = server.arg("state");
  state.toLowerCase();
  if (ch < 1 || ch > 2) {
    server.send(400, "application/json", "{\"error\":\"ch must be 1-2\"}");
    return;
  }
  if (state != "on" && state != "off") {
    server.send(400, "application/json", "{\"error\":\"state must be on or off\"}");
    return;
  }
  setSsr(ch - 1, state == "on");
  server.send(200, "application/json", ssrJson());
}

void handleStatus() {
  touchServed();
  server.send(200, "application/json", ssrJson());
}

#endif

// ---------- Handlers ----------
void handleReading() {
  touchServed();
  if (isnan(lastTemp) || isnan(lastHum)) {
    char body[160];
    snprintf(body, sizeof(body),
      "{\"error\":\"No valid reading yet\",\"sda\":%d,\"scl\":%d,\"fail_count\":%d,\"i2c_recoveries\":%lu}",
      sdaPin, sclPin, failCount, i2cRecoveries);
    server.send(503, "application/json", body);
    return;
  }

  unsigned long ageSeconds = (millis() - lastGoodReadTime) / 1000;
  char payload[360];
  snprintf(payload, sizeof(payload),
    "{\"temperature_c\":%.2f,\"humidity_percent\":%.1f,\"age_seconds\":%lu,\"fail_count\":%d,\"success_count\":%d,"
    "\"i2c_recoveries\":%lu,\"free_heap\":%u,\"min_free_heap\":%u,\"rssi\":%d,\"uptime_s\":%lu,\"reset_reason\":%d}",
    lastTemp, lastHum, ageSeconds, failCount, successCount,
    i2cRecoveries, ESP.getFreeHeap(), ESP.getMinFreeHeap(), WiFi.RSSI(),
    millis() / 1000, (int)esp_reset_reason());
  server.send(200, "application/json", payload);
}

void handleRoot() {
  touchServed();
  server.send(200, "text/plain",
#ifdef NO_SSR
    "ESP32 SHT20 node.\nGET /reading");
#else
    "ESP32 SHT20 + SSR node.\nGET /reading\nGET /relay?ch=1&state=on   (ch = 1-2)\nGET /status");
#endif
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

#ifndef NO_SSR
  // SSR outputs off before enabling the pins, so nothing flicks on at boot
  for (int i = 0; i < 2; i++) {
    digitalWrite(SSR_PINS[i], SSR_OFF);
    pinMode(SSR_PINS[i], OUTPUT);
  }
  // ...then restored to the last commanded state.
  prefs.begin("dht", false);
  uint8_t bits = prefs.getUChar("ssr", 0);
  for (int i = 0; i < 2; i++) {
    ssrState[i] = bits & (1 << i);
    digitalWrite(SSR_PINS[i], ssrState[i] ? SSR_ON : SSR_OFF);
  }
  Serial.printf("restored SSR 0x%02x\n", bits);
#endif
  
  probeI2c();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);  // modem sleep makes some APs drop unicast to this board
  addNetworks();

  Serial.print("Connecting to WiFi");
  for (int attempts = 0; attempts < 3 && wifiMulti.run(8000) != WL_CONNECTED; attempts++)
    Serial.print(".");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi FAILED. Status: " + String(WiFi.status()));
  } else {
    Serial.println();
    Serial.printf("on %s, IP address: ", WiFi.SSID().c_str());
    Serial.println(WiFi.localIP());
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiUp = true;
    startMdns();
  }

  server.on("/", handleRoot);
  server.on("/reading", handleReading);
#ifndef NO_SSR
  server.on("/relay", handleRelay);
  server.on("/status", handleStatus);
#endif
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
  touchServed();

  delay(2000);
  updateSensor();
  startHardwareWatchdog();   // after the Wi-Fi wait above
}

// ---------- Loop ----------
void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  updateSensor();
  ensureWifi();
  checkWatchdog();
}

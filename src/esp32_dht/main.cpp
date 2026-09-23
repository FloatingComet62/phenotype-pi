#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Wire.h>
#include "../secrets.h"
#include <Preferences.h>

// ---------- Config ----------
const char* WIFI_SSID = WIFI_SSID_HOME;
const char* WIFI_PASSWORD = WIFI_PASSWORD_HOME;
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

// ---------- Background sensor update ----------
void updateSensor() {
  unsigned long now = millis();
  if (now - lastReadTime < READ_INTERVAL) return;
  lastReadTime = now;

  float t = readSHT20(TEMP_CMD);
  float h = readSHT20(HUM_CMD);

  if (isnan(t) || isnan(h)) {
    failCount++;
    return;
  }

  // Sanity check: SHT20 valid range
  if (t < -40 || t > 125 || h < 0 || h > 100) {
    failCount++;
    return;
  }

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

void checkWatchdog() {
  if (millis() - lastServed > SERVE_TIMEOUT) {
    Serial.println("no HTTP request served in 5 min, rebooting");
    delay(100);
    ESP.restart();
  }
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
    WiFi.reconnect();
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
    server.send(503, "application/json", "{\"error\":\"No valid reading yet\"}");
    return;
  }

  unsigned long ageSeconds = (millis() - lastGoodReadTime) / 1000;
  char payload[200];
  snprintf(payload, sizeof(payload),
    "{\"temperature_c\":%.2f,\"humidity_percent\":%.1f,\"age_seconds\":%lu,\"fail_count\":%d,\"success_count\":%d}",
    lastTemp, lastHum, ageSeconds, failCount, successCount);
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
  
  // Boards have been wired to either SDA/SCL pair; probe for the SHT20 (0x40)
  // on each at boot and keep the first that ACKs. Falls back to 18/19.
  const int I2C_PAIRS[][2] = {{18, 19}, {21, 22}};
  bool found = false;
  for (auto& pr : I2C_PAIRS) {
    Wire.end();
    Wire.begin(pr[0], pr[1]);
    Wire.setClock(100000);  // SHT20 max is 400kHz, but 100kHz is safer
    Wire.beginTransmission(SHT20_ADDR);
    if (Wire.endTransmission() == 0) {
      Serial.printf("SHT20 found on SDA=%d SCL=%d\n", pr[0], pr[1]);
      found = true;
      break;
    }
    Serial.printf("no SHT20 on SDA=%d SCL=%d\n", pr[0], pr[1]);
  }
  if (!found) {
    Wire.end();
    Wire.begin(18, 19);
    Wire.setClock(100000);
    Serial.println("SHT20 not found on either pair; check wiring");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);  // modem sleep makes some APs drop unicast to this board
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(300);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi FAILED. Status: " + String(WiFi.status()));
  } else {
    Serial.println();
    Serial.print("IP address: ");
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
}

// ---------- Loop ----------
void loop() {
  server.handleClient();
  updateSensor();
  ensureWifi();
  checkWatchdog();
}

// Relay board: 4 relays (water valves) + the AC output, as an HTTP server on
// the room Wi-Fi.
// The Pi's edge service is the only intended client (POST /relay-proxy there
// becomes GET /relay, /ac and /status here).
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <driver/gpio.h>
#include "../secrets.h"

// ---------- Config ----------
const char* WIFI_SSID     = WIFI_SSID_HOME;
const char* WIFI_PASSWORD = WIFI_PASSWORD_HOME;
const char* HOSTNAME      = "relay";   // -> http://relay.local, matches RELAY_HOST on the Pi

// 4-channel relay GPIOs, one water valve each — change to match your wiring
const int RELAY_PINS[4] = {16, 17, 18, 19};
const int NUM_RELAYS = 4;

// Dedicated AC control pin. The AC contact is wired inverted (output released
// = AC running); the Pi's relay proxy compensates, this sketch does not.
const int AC_PIN = 23;

// Active-low: LOW = ON, HIGH = OFF
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// Transmit power. The board sits a few metres from the access point, and full
// power (19.5 dBm) loads the 3.3 V regulator for nothing. Raise it with
// -DWIFI_TX_POWER=WIFI_POWER_19_5dBm in platformio.ini if the link is weak.
#ifndef WIFI_TX_POWER
#define WIFI_TX_POWER WIFI_POWER_13dBm
#endif

// Recovery timings, before backoff (see restartMultiplier()).
const unsigned long WIFI_CHECK_INTERVAL = 15000;               // look at the link
const unsigned long WIFI_SOFT_RESET_MS  = 30000;               // restart the Wi-Fi driver
const unsigned long WIFI_RESTART_MS     = 60000;               // restart the chip
const unsigned long SERVE_TIMEOUT       = 5UL * 60UL * 1000UL; // no request served

WebServer server(80);
Preferences prefs;
bool relayState[4] = {false, false, false, false}; // logical state, true = ON
bool acState = false;
uint8_t restarts = 0;   // consecutive self-restarts with nothing served in between

// ---------- Outputs ----------
// Output states live in flash. Any reboot of this board would otherwise
// release the AC output, which with the inverted wiring switches the AC on.
void saveStates() {
  uint8_t bits = 0;
  for (int i = 0; i < NUM_RELAYS; i++) if (relayState[i]) bits |= 1 << i;
  if (acState) bits |= 1 << 7;
  prefs.putUChar("out", bits);
}

void setRelay(int index, bool on) {
  if (index < 0 || index >= NUM_RELAYS) return;
  relayState[index] = on;
  digitalWrite(RELAY_PINS[index], on ? RELAY_ON : RELAY_OFF);
  saveStates();
}

void setAC(bool on) {
  acState = on;
  digitalWrite(AC_PIN, on ? RELAY_ON : RELAY_OFF);
  saveStates();
}

void restoreOutputs() {
  uint8_t bits = prefs.getUChar("out", 0);
  for (int i = 0; i <= NUM_RELAYS; i++) {
    // Relays 1-4 drive water valves: they always start closed, whatever they
    // were before the restart. Only the AC returns to its saved state.
    bool on = i < NUM_RELAYS ? false : bits & (1 << 7);
    int pin = i < NUM_RELAYS ? RELAY_PINS[i] : AC_PIN;
    if (i < NUM_RELAYS) relayState[i] = on; else acState = on;
    // Level first, then drive the pin, then release any hold left by
    // plannedRestart(): the relay goes straight to its saved state.
    digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
    pinMode(pin, OUTPUT);
    gpio_hold_dis((gpio_num_t)pin);
  }
  Serial.printf("restored outputs 0x%02x\n", bits);
}

// ---------- Self-restart with backoff ----------
// A restart is the only cure for a hung Wi-Fi stack, but this board switches
// a compressor, so it must not restart in a loop while the router or the Pi
// is simply off. Each restart that is not followed by a served request
// doubles both timeouts, up to 16x (Wi-Fi 16 min, no-request 80 min).
unsigned long restartMultiplier() { return 1UL << min<uint8_t>(restarts, 4); }

void plannedRestart(const char* why) {
  Serial.printf("restarting: %s (restart #%u)\n", why, restarts + 1);
  prefs.putUChar("rst", min<uint8_t>(restarts + 1, 200));
  // Latch the pads so the relays do not click while the chip resets.
  for (int i = 0; i < NUM_RELAYS; i++) gpio_hold_en((gpio_num_t)RELAY_PINS[i]);
  gpio_hold_en((gpio_num_t)AC_PIN);
  WiFi.disconnect(true);
  delay(1000);
  ESP.restart();
}

unsigned long lastServed = 0;

void touchServed() {
  lastServed = millis();
  if (restarts) {
    restarts = 0;
    prefs.putUChar("rst", 0);
  }
}

void checkServeWatchdog() {
  // The Pi GETs /status every minute. Silence this long while we believe we
  // are online is the state where the board answers ARP but no TCP.
  if (millis() - lastServed > SERVE_TIMEOUT * restartMultiplier())
    plannedRestart("no request served");
}

// ---------- Wi-Fi ----------
void startMdns() {
  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS responder started: http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("Error starting mDNS");
  }
}

void beginWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);     // DHCP; the router owns the address
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);           // modem sleep gets boards dropped by the AP
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setTxPower(WIFI_TX_POWER);
}

bool wifiUp = false;
bool softResetDone = false;
unsigned long lastWifiCheck = 0;
unsigned long wifiDownSince = 0;

void ensureWifi() {
  unsigned long now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) {
      Serial.print("WiFi up, IP: ");
      Serial.println(WiFi.localIP());
      startMdns();   // the responder does not survive a reconnect
    }
    wifiUp = true;
    softResetDone = false;
    return;
  }

  if (wifiUp || wifiDownSince == 0) wifiDownSince = now;
  wifiUp = false;
  unsigned long downFor = now - wifiDownSince;

  if (downFor > WIFI_RESTART_MS * restartMultiplier()) {
    plannedRestart("Wi-Fi would not reconnect");
  } else if (downFor > WIFI_SOFT_RESET_MS && !softResetDone) {
    Serial.println("WiFi down 30 s, restarting the Wi-Fi driver");
    softResetDone = true;
    WiFi.disconnect(true);
    delay(500);
    beginWifi();
  } else {
    Serial.println("WiFi down, reconnecting");
    WiFi.reconnect();
  }
}

// ---------- HTTP ----------
String statusJson() {
  String json = "{";
  for (int i = 0; i < NUM_RELAYS; i++) {
    json += "\"relay" + String(i + 1) + "\":" + (relayState[i] ? "true" : "false") + ",";
  }
  json += "\"ac\":" + String(acState ? "true" : "false");
  // Diagnostics. The Pi only reads the keys above.
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"uptime_s\":" + String(millis() / 1000);
  json += ",\"restarts\":" + String(restarts);
  json += "}";
  return json;
}

bool readState(bool& on) {
  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"missing state param\"}");
    return false;
  }
  String state = server.arg("state");
  state.toLowerCase();
  if (state != "on" && state != "off") {
    server.send(400, "application/json", "{\"error\":\"state must be on or off\"}");
    return false;
  }
  on = state == "on";
  return true;
}

void handleRoot() {
  touchServed();
  server.send(200, "text/plain",
    "ESP32 4-channel relay + AC control server.\n"
    "GET /relay?ch=1&state=on   (ch = 1-4, state = on/off)\n"
    "GET /all?state=on          (turn all 4 relays on/off, AC unaffected)\n"
    "GET /ac?state=on           (control AC only)\n"
    "GET /status                (current state of all outputs)");
}

void handleRelay() {
  touchServed();
  if (!server.hasArg("ch")) {
    server.send(400, "application/json", "{\"error\":\"missing ch or state param\"}");
    return;
  }
  int ch = server.arg("ch").toInt();
  if (ch < 1 || ch > NUM_RELAYS) {
    server.send(400, "application/json", "{\"error\":\"ch must be 1-4\"}");
    return;
  }
  bool on;
  if (!readState(on)) return;
  setRelay(ch - 1, on);
  server.send(200, "application/json", statusJson());
}

void handleAll() {
  touchServed();
  bool on;
  if (!readState(on)) return;
  for (int i = 0; i < NUM_RELAYS; i++) setRelay(i, on);
  // AC deliberately excluded from /all — control it explicitly via /ac
  server.send(200, "application/json", statusJson());
}

void handleAC() {
  touchServed();
  bool on;
  if (!readState(on)) return;
  setAC(on);
  server.send(200, "application/json", statusJson());
}

void handleStatus() {
  touchServed();
  server.send(200, "application/json", statusJson());
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

  // Outputs before anything else, so the relays spend as little time as
  // possible in the reset state.
  prefs.begin("relay", false);
  restoreOutputs();
  restarts = prefs.getUChar("rst", 0);
  Serial.printf("consecutive self-restarts: %u\n", restarts);

  beginWifi();
  Serial.print("Connecting to WiFi");
  for (int attempts = 0; WiFi.status() != WL_CONNECTED && attempts < 40; attempts++) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    wifiUp = true;
    startMdns();
  } else {
    Serial.println("\nWiFi FAILED. Status: " + String(WiFi.status()));
  }

  server.on("/", handleRoot);
  server.on("/relay", handleRelay);
  server.on("/all", handleAll);
  server.on("/ac", handleAC);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
  lastServed = millis();   // start the no-request clock without clearing the restart count
}

// ---------- Loop ----------
void loop() {
  server.handleClient();
  ensureWifi();
  checkServeWatchdog();
}

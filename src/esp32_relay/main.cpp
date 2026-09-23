#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include "../secrets.h"

// ---------- Config ----------
const char* WIFI_SSID     = WIFI_SSID_HOME;
const char* WIFI_PASSWORD = WIFI_PASSWORD_HOME;
const char* HOSTNAME      = "esp32-relay";   // -> http://esp32-relay.local

// 4-channel relay GPIOs — change to match your wiring
const int RELAY_PINS[4] = {16, 17, 18, 19};
const int NUM_RELAYS = 4;

// Dedicated AC control pin (separate relay/IR/contactor trigger)
const int AC_PIN = 23;

// Active-low: LOW = ON, HIGH = OFF
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

WebServer server(80);
// Output states survive a reboot. With the AC contact wired inverted, a
// brownout that reboots this board would otherwise switch the AC on by
// itself; restoring the last commanded state closes that hole.
Preferences prefs;
bool relayState[4] = {false, false, false, false}; // logical state, true = ON
bool acState = false;

// ---------- Helpers ----------
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

// ---------- Wi-Fi keepalive (same as the sensor boards) ----------
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

String statusJson() {
  String json = "{";
  for (int i = 0; i < NUM_RELAYS; i++) {
    json += "\"relay" + String(i + 1) + "\":" + (relayState[i] ? "true" : "false") + ",";
  }
  json += "\"ac\":" + String(acState ? "true" : "false");
  json += "}";
  return json;
}

// ---------- Handlers ----------
void handleRoot() {
  server.send(200, "text/plain",
    "ESP32 4-channel relay + AC control server.\n"
    "GET /relay?ch=1&state=on   (ch = 1-4, state = on/off)\n"
    "GET /all?state=on          (turn all 4 relays on/off, AC unaffected)\n"
    "GET /ac?state=on           (control AC only)\n"
    "GET /status                (current state of all outputs)");
}

void handleRelay() {
  if (!server.hasArg("ch") || !server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"missing ch or state param\"}");
    return;
  }

  int ch = server.arg("ch").toInt();
  String state = server.arg("state");
  state.toLowerCase();

  if (ch < 1 || ch > NUM_RELAYS) {
    server.send(400, "application/json", "{\"error\":\"ch must be 1-4\"}");
    return;
  }
  if (state != "on" && state != "off") {
    server.send(400, "application/json", "{\"error\":\"state must be on or off\"}");
    return;
  }

  setRelay(ch - 1, state == "on");
  server.send(200, "application/json", statusJson());
}

void handleAll() {
  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"missing state param\"}");
    return;
  }
  String state = server.arg("state");
  state.toLowerCase();
  if (state != "on" && state != "off") {
    server.send(400, "application/json", "{\"error\":\"state must be on or off\"}");
    return;
  }

  for (int i = 0; i < NUM_RELAYS; i++) setRelay(i, state == "on");
  // AC deliberately excluded from /all — control it explicitly via /ac
  server.send(200, "application/json", statusJson());
}

void handleAC() {
  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"missing state param\"}");
    return;
  }
  String state = server.arg("state");
  state.toLowerCase();
  if (state != "on" && state != "off") {
    server.send(400, "application/json", "{\"error\":\"state must be on or off\"}");
    return;
  }

  setAC(state == "on");
  server.send(200, "application/json", statusJson());
}

void handleStatus() {
  server.send(200, "application/json", statusJson());
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

  // Initialize all outputs OFF *before* declaring as OUTPUT
  for (int i = 0; i < NUM_RELAYS; i++) {
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
    pinMode(RELAY_PINS[i], OUTPUT);
    relayState[i] = false;
  }
  digitalWrite(AC_PIN, RELAY_OFF);
  pinMode(AC_PIN, OUTPUT);
  acState = false;

  // Restore the last commanded outputs (see Preferences note above).
  prefs.begin("relay", false);
  uint8_t bits = prefs.getUChar("out", 0);
  for (int i = 0; i < NUM_RELAYS; i++) {
    relayState[i] = bits & (1 << i);
    digitalWrite(RELAY_PINS[i], relayState[i] ? RELAY_ON : RELAY_OFF);
  }
  acState = bits & (1 << 7);
  digitalWrite(AC_PIN, acState ? RELAY_ON : RELAY_OFF);
  Serial.printf("restored outputs 0x%02x\n", bits);

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
  server.on("/relay", handleRelay);
  server.on("/all", handleAll);
  server.on("/ac", handleAC);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

// ---------- Loop ----------
void loop() {
  server.handleClient();
  ensureWifi();
}

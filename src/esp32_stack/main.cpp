// Stack controller bridge: Pi <-- HTTP over Wi-Fi --> this ESP32 <-- UART --> Arduino.
//
// The Arduino drives one stack: three WS2812B strips (paths 0-2), the pump
// PWM (path 3), and reads pump current and tank pH. It speaks a line
// protocol at 115200 baud (references: the protocol table, 28 Sep 2026):
//
//   ESP32 -> Arduino   "path,start,end,r,g,b,brightness\n"   path 0-2
//                      "3,speed\n"                           pump, 0-255
//   Arduino -> ESP32   "3,volts\n"  "4,ph\n"                 once a second
//
// Strip layout on a stack (2 shelves x 4 rows), as wired on site:
//   path 0 = shelf 1, rows 1-3      path 1 = shelf 2, rows 1-3
//   path 2 = shelf 1 row 4, then shelf 2 row 4
// A row is a start..end range of LED indexes on its strip. This board does
// not know the ranges; whoever calls /led supplies them.
//
// Wiring (change the two pins below if yours differ):
//   Arduino TX (pin 1) -> ESP32 GPIO 16 (RX2)  ** through a divider: the
//                         Arduino is 5 V, the ESP32 pin is 3.3 V only **
//   Arduino RX (pin 0) <- ESP32 GPIO 17 (TX2)  direct
//   GND               <-> GND
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include "../secrets.h"

// ---------- Config ----------
const char* WIFI_SSID     = WIFI_SSID_HOME;
const char* WIFI_PASSWORD = WIFI_PASSWORD_HOME;

#ifndef HOSTNAME_STR
#define HOSTNAME_STR "stack1"   // one board per stack: stack1, stack2, ...
#endif
const char* HOSTNAME = HOSTNAME_STR;

const int  UART_RX_PIN = 16;
const int  UART_TX_PIN = 17;
const long UART_BAUD   = 115200;

const int NUM_STRIPS = 3;      // paths 0-2
const int NUM_LEDS   = 300;    // per strip, must match the Arduino
const int PUMP_PATH  = 3;
const int PH_PATH    = 4;

// The Arduino is deaf while FastLED.show() clocks out 900 LEDs (~30 ms with
// interrupts off), so two commands sent back to back arrive corrupted.
// Every command waits this long after the previous one.
const unsigned long COMMAND_GAP_MS = 150;

// Telemetry arrives every second. Older than this and it is reported stale.
const unsigned long TELEMETRY_STALE_MS = 5000;

#ifndef WIFI_TX_POWER
#define WIFI_TX_POWER WIFI_POWER_13dBm
#endif
const unsigned long WIFI_CHECK_INTERVAL = 15000;
const unsigned long WIFI_SOFT_RESET_MS  = 30000;
const unsigned long WIFI_RESTART_MS     = 60000;
const unsigned long SERVE_TIMEOUT       = 5UL * 60UL * 1000UL;

WebServer server(80);
Preferences prefs;
HardwareSerial& arduino = Serial2;

// ---------- State ----------
float motorVolts = NAN, ph = NAN;
unsigned long motorAt = 0, phAt = 0;      // millis() of the last good line
unsigned long linesOk = 0, linesBad = 0, commandsSent = 0;

// Last thing we told the Arduino. It never acknowledges, so this is what was
// sent, not what happened. -1 = nothing sent since this board started.
int pumpSpeed = -1;
struct Led { int start, end, r, g, b, brightness; };
Led lastLed[NUM_STRIPS] = {{-1, 0, 0, 0, 0, 0}, {-1, 0, 0, 0, 0, 0}, {-1, 0, 0, 0, 0, 0}};

uint8_t restarts = 0;

// ---------- Arduino link ----------
// "3,1.35" -> path 3, value 1.35. Anything else (boot noise, a torn line) is
// rejected rather than guessed at.
bool parseTelemetry(const char* line, int& path, float& value) {
  char* end;
  long p = strtol(line, &end, 10);
  if (end == line || *end != ',') return false;
  const char* rest = end + 1;
  float v = strtof(rest, &end);
  if (end == rest || *end != '\0' || isnan(v) || isinf(v)) return false;
  path = (int)p;
  value = v;
  return true;
}

void readArduino() {
  static char buf[48];
  static size_t len = 0;
  while (arduino.available()) {
    char c = arduino.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < sizeof(buf) - 1) buf[len++] = c;
      else len = sizeof(buf);           // overlong: poison until the newline
      continue;
    }
    bool overlong = len >= sizeof(buf);
    buf[overlong ? 0 : len] = '\0';
    len = 0;
    int path; float value;
    if (overlong || !parseTelemetry(buf, path, value)) { linesBad++; continue; }
    if (path == PUMP_PATH && value >= 0 && value <= 5.5) {
      motorVolts = value; motorAt = millis(); linesOk++;
    } else if (path == PH_PATH && value >= -1 && value <= 15) {
      ph = value; phAt = millis(); linesOk++;
    } else {
      linesBad++;
    }
  }
}

void sendCommand(const String& line) {
  static unsigned long lastSent = 0;
  while (millis() - lastSent < COMMAND_GAP_MS) { readArduino(); delay(1); }
  arduino.print(line);
  arduino.print('\n');
  arduino.flush();
  lastSent = millis();
  commandsSent++;
  Serial.printf("-> arduino: %s\n", line.c_str());
}

// ---------- Self-restart with backoff (same scheme as the relay board) ----------
unsigned long restartMultiplier() { return 1UL << min<uint8_t>(restarts, 4); }

void plannedRestart(const char* why) {
  Serial.printf("restarting: %s (restart #%u)\n", why, restarts + 1);
  prefs.putUChar("rst", min<uint8_t>(restarts + 1, 200));
  WiFi.disconnect(true);
  delay(1000);
  ESP.restart();   // the Arduino keeps its LEDs and pump as they are
}

unsigned long lastServed = 0;
void touchServed() {
  lastServed = millis();
  if (restarts) { restarts = 0; prefs.putUChar("rst", 0); }
}

void checkServeWatchdog() {
  if (millis() - lastServed > SERVE_TIMEOUT * restartMultiplier())
    plannedRestart("no request served");
}

// ---------- Wi-Fi ----------
void startMdns() {
  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS responder started: http://%s.local\n", HOSTNAME);
  }
}

void beginWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setTxPower(WIFI_TX_POWER);
}

bool wifiUp = false, softResetDone = false;
unsigned long lastWifiCheck = 0, wifiDownSince = 0;

void ensureWifi() {
  unsigned long now = millis();
  if (now - lastWifiCheck < WIFI_CHECK_INTERVAL) return;
  lastWifiCheck = now;
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) {
      Serial.print("WiFi up, IP: ");
      Serial.println(WiFi.localIP());
      startMdns();
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
    softResetDone = true;
    WiFi.disconnect(true);
    delay(500);
    beginWifi();
  } else {
    WiFi.reconnect();
  }
}

// ---------- HTTP ----------
String num(float v, int decimals) { return isnan(v) ? String("null") : String(v, decimals); }
long ageSeconds(unsigned long at) { return at ? (long)((millis() - at) / 1000) : -1; }
bool fresh(unsigned long at) { return at && millis() - at < TELEMETRY_STALE_MS; }

String readingJson() {
  return "{\"ph\":" + num(ph, 2) +
         ",\"ph_age_seconds\":" + String(ageSeconds(phAt)) +
         ",\"motor_voltage\":" + num(motorVolts, 2) +
         ",\"motor_age_seconds\":" + String(ageSeconds(motorAt)) +
         ",\"arduino_online\":" + (fresh(phAt) || fresh(motorAt) ? "true" : "false") + "}";
}

String statusJson() {
  String json = readingJson();
  json.remove(json.length() - 1);
  json += ",\"pump_speed\":" + (pumpSpeed < 0 ? String("null") : String(pumpSpeed));
  json += ",\"leds\":[";
  for (int i = 0; i < NUM_STRIPS; i++) {
    const Led& l = lastLed[i];
    if (i) json += ",";
    if (l.start < 0) { json += "null"; continue; }
    json += "{\"start\":" + String(l.start) + ",\"end\":" + String(l.end) +
            ",\"r\":" + String(l.r) + ",\"g\":" + String(l.g) + ",\"b\":" + String(l.b) +
            ",\"brightness\":" + String(l.brightness) + "}";
  }
  json += "]";
  json += ",\"lines_ok\":" + String(linesOk) + ",\"lines_bad\":" + String(linesBad);
  json += ",\"commands_sent\":" + String(commandsSent);
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += ",\"uptime_s\":" + String(millis() / 1000);
  json += ",\"restarts\":" + String(restarts) + "}";
  return json;
}

void fail(const String& why) {
  server.send(400, "application/json", "{\"error\":\"" + why + "\"}");
}

// Reads an integer query parameter and range-checks it. `fallback` < lo
// means the parameter is required.
bool intArg(const char* name, int lo, int hi, int fallback, int& out) {
  if (!server.hasArg(name)) {
    if (fallback < lo) { fail(String("missing ") + name); return false; }
    out = fallback;
    return true;
  }
  String raw = server.arg(name);
  char* end;
  long v = strtol(raw.c_str(), &end, 10);
  if (raw.length() == 0 || *end != '\0' || v < lo || v > hi) {
    fail(String(name) + " must be " + lo + "-" + hi);
    return false;
  }
  out = (int)v;
  return true;
}

void handleRoot() {
  touchServed();
  server.send(200, "text/plain",
    "ESP32 stack controller bridge (Pi <-> Arduino over UART).\n"
    "GET /reading                       pH and pump current sense\n"
    "GET /status                        readings + last commands + diagnostics\n"
    "GET /led?path=0&r=255&g=0&b=0      path 0-2; optional start,end (0-299),\n"
    "                                   brightness (0-255, default 255)\n"
    "GET /pump?speed=180                0-255, 0 = stop\n");
}

void handleReading() {
  touchServed();
  if (!fresh(phAt) && !fresh(motorAt)) {
    server.send(503, "application/json",
      "{\"error\":\"No telemetry from the Arduino\",\"lines_bad\":" + String(linesBad) + "}");
    return;
  }
  server.send(200, "application/json", readingJson());
}

void handleStatus() {
  touchServed();
  server.send(200, "application/json", statusJson());
}

void handleLed() {
  touchServed();
  int path, start, end, r, g, b, brightness;
  if (!intArg("path", 0, NUM_STRIPS - 1, -1, path)) return;
  if (!intArg("start", 0, NUM_LEDS - 1, 0, start)) return;
  if (!intArg("end", 0, NUM_LEDS - 1, NUM_LEDS - 1, end)) return;
  if (!intArg("r", 0, 255, -1, r)) return;
  if (!intArg("g", 0, 255, -1, g)) return;
  if (!intArg("b", 0, 255, -1, b)) return;
  if (!intArg("brightness", 0, 255, 255, brightness)) return;
  if (start > end) { int t = start; start = end; end = t; }

  char line[48];
  snprintf(line, sizeof(line), "%d,%d,%d,%d,%d,%d,%d", path, start, end, r, g, b, brightness);
  sendCommand(line);
  lastLed[path] = {start, end, r, g, b, brightness};
  server.send(200, "application/json", statusJson());
}

void handlePump() {
  touchServed();
  int speed;
  if (!intArg("speed", 0, 255, -1, speed)) return;
  sendCommand(String(PUMP_PATH) + "," + speed);
  pumpSpeed = speed;
  server.send(200, "application/json", statusJson());
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ---------- Setup ----------
void selfTest() {
  int p; float v;
  bool ok = parseTelemetry("3,1.35", p, v) && p == 3 && fabs(v - 1.35) < 0.001
         && parseTelemetry("4,6.84", p, v) && p == 4
         && !parseTelemetry("", p, v) && !parseTelemetry("4", p, v)
         && !parseTelemetry("4,", p, v) && !parseTelemetry("4,6.8x", p, v)
         && !parseTelemetry("x,1", p, v) && !parseTelemetry("4,nan", p, v);
  Serial.println(ok ? "self-test ok" : "SELF-TEST FAILED: telemetry parser");
}

void setup() {
  Serial.begin(115200);
  selfTest();
  arduino.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  prefs.begin("stack", false);
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
  server.on("/reading", handleReading);
  server.on("/status", handleStatus);
  server.on("/led", handleLed);
  server.on("/pump", handlePump);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
  lastServed = millis();
}

// ---------- Loop ----------
void loop() {
  readArduino();
  server.handleClient();
  ensureWifi();
  checkServeWatchdog();
}

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
#include <esp_task_wdt.h>
#include "../secrets.h"

// What happens when something dies:
//   Wi-Fi drops          reconnect -> Wi-Fi driver restart (30 s) -> chip
//                        restart (60 s), backing off while nothing is served
//   ESP32 answers ARP    chip restart after 5 min without a served request
//     but no requests
//   ESP32 freezes        hardware watchdog restarts it after 30 s
//   ESP32 restarts       the Arduino keeps LEDs and pump as they were; the
//                        commanded state is read back from flash
//   Arduino resets,      the commanded state is sent again: at once when
//     or a command is      telemetry resumes after a gap, and every minute
//     lost on the wire     regardless (the Arduino never acknowledges)
//   Arduino goes silent  /reading answers 503, /status says arduino_online:false
// Nothing here switches the pump or the LEDs off by itself.

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
// A gap this long between telemetry lines means the Arduino probably reset
// (its bootloader alone takes about 1.5 s) and has forgotten its outputs.
const unsigned long TELEMETRY_GAP_RESET_MS = 2500;
// How often the commanded state is sent again regardless.
const unsigned long REASSERT_INTERVAL_MS = 60000;
const int HARDWARE_WATCHDOG_S = 30;

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

// Commanded state: the last thing the Pi asked for. The Arduino never
// acknowledges, so this is what was sent, not what happened. It lives in
// flash so that it survives a restart of this board.
const int SEGMENTS_PER_STRIP = 4;   // a strip carries at most 3 rows
struct Segment {
  int16_t start, end;
  uint8_t r, g, b, brightness, used;
};
struct Desired {
  uint8_t version;
  int16_t pumpSpeed;                // -1 = never commanded
  Segment seg[NUM_STRIPS][SEGMENTS_PER_STRIP];
  uint8_t next[NUM_STRIPS];         // slot to recycle when a strip is full
};
const uint8_t DESIRED_VERSION = 1;
Desired desired;

void clearDesired(Desired& d) {
  memset(&d, 0, sizeof(d));
  d.version = DESIRED_VERSION;
  d.pumpSpeed = -1;
}

// Records a segment. A new segment replaces the one with the same range and
// swallows any it fully covers, so "whole strip red" wipes the three rows
// stored before it and they are not sent again afterwards.
void rememberSegment(Desired& d, int path, const Segment& incoming) {
  Segment* slots = d.seg[path];
  int target = -1;
  for (int i = 0; i < SEGMENTS_PER_STRIP; i++) {
    if (!slots[i].used) continue;
    if (slots[i].start >= incoming.start && slots[i].end <= incoming.end) {
      slots[i].used = 0;
      if (target < 0) target = i;
    }
  }
  for (int i = 0; target < 0 && i < SEGMENTS_PER_STRIP; i++)
    if (!slots[i].used) target = i;
  if (target < 0) {
    target = d.next[path];
    d.next[path] = (d.next[path] + 1) % SEGMENTS_PER_STRIP;
  }
  slots[target] = incoming;
  slots[target].used = 1;
}

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

unsigned long lastLineAt = 0, arduinoGaps = 0;
void startReassert();

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
    bool good = true;
    if (path == PUMP_PATH && value >= 0 && value <= 5.5) {
      motorVolts = value; motorAt = millis();
    } else if (path == PH_PATH && value >= -1 && value <= 15) {
      ph = value; phAt = millis();
    } else {
      good = false;
    }
    if (!good) { linesBad++; continue; }
    linesOk++;
    unsigned long now = millis();
    if (lastLineAt && now - lastLineAt > TELEMETRY_GAP_RESET_MS) {
      Serial.println("telemetry resumed after a gap, sending the commanded state again");
      arduinoGaps++;
      startReassert();
    }
    lastLineAt = now;
  }
}

unsigned long lastSentAt = 0;

bool canSend() { return millis() - lastSentAt >= COMMAND_GAP_MS; }

void writeCommand(const String& line) {
  arduino.print(line);
  arduino.print('\n');
  arduino.flush();
  lastSentAt = millis();
  commandsSent++;
  Serial.printf("-> arduino: %s\n", line.c_str());
}

void sendCommand(const String& line) {
  while (!canSend()) { readArduino(); delay(1); }
  writeCommand(line);
}

String pumpLine(int speed) { return String(PUMP_PATH) + "," + speed; }

String segmentLine(int path, const Segment& g) {
  char line[48];
  snprintf(line, sizeof(line), "%d,%d,%d,%d,%d,%d,%d",
           path, g.start, g.end, g.r, g.g, g.b, g.brightness);
  return line;
}

// Re-sending is spread over loop() passes, one command per gap, so that HTTP
// requests are still served while it runs. Item 0 is the pump, then every
// segment slot in turn.
const int REASSERT_ITEMS = 1 + NUM_STRIPS * SEGMENTS_PER_STRIP;
int reassertAt = REASSERT_ITEMS;   // == REASSERT_ITEMS: idle
unsigned long lastReassert = 0, reasserts = 0;

void startReassert() {
  reassertAt = 0;
  lastReassert = millis();
  reasserts++;
}

void reassertStep() {
  if (reassertAt >= REASSERT_ITEMS) {
    if (millis() - lastReassert > REASSERT_INTERVAL_MS) startReassert();
    return;
  }
  if (!canSend()) return;
  while (reassertAt < REASSERT_ITEMS) {
    int item = reassertAt++;
    if (item == 0) {
      if (desired.pumpSpeed < 0) continue;
      writeCommand(pumpLine(desired.pumpSpeed));
      return;
    }
    int path = (item - 1) / SEGMENTS_PER_STRIP, slot = (item - 1) % SEGMENTS_PER_STRIP;
    if (!desired.seg[path][slot].used) continue;
    writeCommand(segmentLine(path, desired.seg[path][slot]));
    return;
  }
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

// Why the access point last dropped or refused us. The status code alone
// (4, "connect failed") does not say; the reason does, e.g. 15 = wrong
// password, 201 = network not found, 5 = access point full.
uint8_t wifiReason = 0;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  wifiReason = info.wifi_sta_disconnected.reason;
  Serial.printf("WiFi disconnected, reason %u (%s)\n", wifiReason,
                WiFi.disconnectReasonName((wifi_err_reason_t)wifiReason));
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

// ---------- Commanded state in flash ----------
void saveDesired() { prefs.putBytes("want", &desired, sizeof(desired)); }

void loadDesired() {
  Desired stored;
  bool ok = prefs.getBytes("want", &stored, sizeof(stored)) == sizeof(stored)
         && stored.version == DESIRED_VERSION;
  if (ok) desired = stored; else clearDesired(desired);
  Serial.printf("commanded state %s\n", ok ? "restored from flash" : "empty");
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
  json += ",\"pump_speed\":" + (desired.pumpSpeed < 0 ? String("null") : String(desired.pumpSpeed));
  json += ",\"leds\":[";
  for (int path = 0; path < NUM_STRIPS; path++) {
    if (path) json += ",";
    json += "[";
    bool first = true;
    for (int i = 0; i < SEGMENTS_PER_STRIP; i++) {
      const Segment& g = desired.seg[path][i];
      if (!g.used) continue;
      if (!first) json += ",";
      first = false;
      json += "{\"start\":" + String(g.start) + ",\"end\":" + String(g.end) +
              ",\"r\":" + String(g.r) + ",\"g\":" + String(g.g) + ",\"b\":" + String(g.b) +
              ",\"brightness\":" + String(g.brightness) + "}";
    }
    json += "]";
  }
  json += "]";
  json += ",\"resends\":" + String(reasserts) + ",\"arduino_gaps\":" + String(arduinoGaps);
  json += ",\"lines_ok\":" + String(linesOk) + ",\"lines_bad\":" + String(linesBad);
  json += ",\"commands_sent\":" + String(commandsSent);
  json += ",\"wifi_reason\":" + String(wifiReason);
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
  int path, start, end, r, gr, b, brightness;
  if (!intArg("path", 0, NUM_STRIPS - 1, -1, path)) return;
  if (!intArg("start", 0, NUM_LEDS - 1, 0, start)) return;
  if (!intArg("end", 0, NUM_LEDS - 1, NUM_LEDS - 1, end)) return;
  if (!intArg("r", 0, 255, -1, r)) return;
  if (!intArg("g", 0, 255, -1, gr)) return;
  if (!intArg("b", 0, 255, -1, b)) return;
  if (!intArg("brightness", 0, 255, 255, brightness)) return;
  if (start > end) { int t = start; start = end; end = t; }

  Segment g = {(int16_t)start, (int16_t)end, (uint8_t)r, (uint8_t)gr, (uint8_t)b,
               (uint8_t)brightness, 1};
  sendCommand(segmentLine(path, g));
  rememberSegment(desired, path, g);
  saveDesired();
  server.send(200, "application/json", statusJson());
}

void handlePump() {
  touchServed();
  int speed;
  if (!intArg("speed", 0, 255, -1, speed)) return;
  sendCommand(pumpLine(speed));
  desired.pumpSpeed = speed;
  saveDesired();
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
  Desired d;
  clearDesired(d);
  rememberSegment(d, 0, {0, 99, 1, 1, 1, 255, 1});
  rememberSegment(d, 0, {100, 199, 2, 2, 2, 255, 1});
  rememberSegment(d, 0, {0, 99, 9, 9, 9, 255, 1});          // same range: replaces
  int used = 0, nines = 0;
  for (const Segment& g : d.seg[0]) { used += g.used; nines += g.used && g.r == 9; }
  ok = ok && used == 2 && nines == 1;
  rememberSegment(d, 0, {0, 299, 7, 7, 7, 255, 1});         // whole strip: swallows both
  used = 0;
  for (const Segment& g : d.seg[0]) used += g.used;
  ok = ok && used == 1 && d.pumpSpeed == -1 && !d.seg[1][0].used;
  Serial.println(ok ? "self-test ok" : "SELF-TEST FAILED");
}

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

void setup() {
  Serial.begin(115200);
  selfTest();
  arduino.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  prefs.begin("stack", false);
  restarts = prefs.getUChar("rst", 0);
  Serial.printf("consecutive self-restarts: %u\n", restarts);
  loadDesired();

  WiFi.onEvent(onWifiEvent);
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
  startHardwareWatchdog();   // after the Wi-Fi wait above, which may take 12 s
  startReassert();           // the Arduino may have reset while we were away
}

// ---------- Loop ----------
void loop() {
  esp_task_wdt_reset();
  readArduino();
  server.handleClient();
  reassertStep();
  ensureWifi();
  checkServeWatchdog();
}

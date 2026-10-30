// Sensor board with an AC output (dht4). Written by the owner, flashed by
// them on 28 Sep 2026; kept here as delivered except that the Wi-Fi
// credentials come from the git-ignored src/secrets.h.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Wire.h>
#include <Preferences.h>
#include "../secrets.h"
#include <esp_task_wdt.h>

// ============================================================
// WIFI CONFIG
// ============================================================

const char* WIFI_SSID     = WIFI_SSID_FARM;      // Poly House, 2.4 GHz
const char* WIFI_PASSWORD = WIFI_PASSWORD_FARM;

#ifndef HOSTNAME_STR
#define HOSTNAME_STR "dht4"
#endif

const char* HOSTNAME = HOSTNAME_STR;


// ============================================================
// SHT20
// ============================================================

#define SHT20_ADDR 0x40

#define TEMP_CMD   0xF3
#define HUM_CMD    0xF5


// ============================================================
// AC
// ============================================================
//
// Active HIGH:
//
// HIGH = AC ON
// LOW  = AC OFF
//
// AC control pin = GPIO 23
//

const int AC_PIN = 23;

#define AC_ON  HIGH
#define AC_OFF LOW


// ============================================================
// GLOBALS
// ============================================================

WebServer server(80);
Preferences prefs;

bool acState = false;


// ============================================================
// SENSOR STATE
// ============================================================

float lastTemp = NAN;
float lastHum  = NAN;

unsigned long lastReadTime = 0;
unsigned long lastGoodReadTime = 0;

const unsigned long READ_INTERVAL = 1500;

int failCount = 0;
int successCount = 0;


// ============================================================
// WIFI FAILSAFE
// ============================================================

bool wifiUp = false;

unsigned long lastWifiCheck = 0;
unsigned long wifiDisconnectedSince = 0;

const unsigned long WIFI_CHECK_INTERVAL = 5000;

// If Wi-Fi stays disconnected this long, restart ESP32.
const unsigned long WIFI_RECOVERY_TIMEOUT =
    45UL * 1000UL;


// ============================================================
// FUNCTION DECLARATIONS
// ============================================================

void updateSensor();
void setAC(bool on);
String statusJson();

void handleRoot();
void handleReading();
void handleStatus();
void handleAC();
void handleNotFound();

void startMdns();
void ensureWifi();


// ============================================================
// SHT20 READ
// ============================================================

float readSHT20(uint8_t command) {

    Wire.beginTransmission(SHT20_ADDR);
    Wire.write(command);

    if (Wire.endTransmission() != 0) {
        return NAN;
    }

    // SHT20 measurement time
    delay(85);

    if (Wire.requestFrom(SHT20_ADDR, 3) != 3) {
        return NAN;
    }

    uint16_t raw =
        ((uint16_t)Wire.read() << 8) |
        Wire.read();

    // Checksum byte
    uint8_t checksum = Wire.read();
    (void)checksum;

    // Clear status bits
    raw &= 0xFFFC;

    if (command == TEMP_CMD) {

        // Temperature in °C
        return -46.85 +
               175.72 * (raw / 65536.0);

    } else {

        // Relative humidity %
        return -6.0 +
               125.0 * (raw / 65536.0);
    }
}


// ============================================================
// SENSOR UPDATE
// ============================================================

void updateSensor() {

    unsigned long now = millis();

    if (now - lastReadTime < READ_INTERVAL) {
        return;
    }

    lastReadTime = now;

    float temperature = readSHT20(TEMP_CMD);
    float humidity    = readSHT20(HUM_CMD);

    if (isnan(temperature) ||
        isnan(humidity)) {

        failCount++;

        Serial.println(
            "SHT20 read failed"
        );

        return;
    }

    // Sanity check
    if (temperature < -40 ||
        temperature > 125 ||
        humidity < 0 ||
        humidity > 100) {

        failCount++;

        Serial.println(
            "SHT20 invalid reading"
        );

        return;
    }

    lastTemp = temperature;
    lastHum = humidity;

    lastGoodReadTime = now;

    successCount++;
}


// ============================================================
// AC CONTROL
// ============================================================

void saveACState() {

    prefs.putBool(
        "ac",
        acState
    );
}


void setAC(bool on) {

    acState = on;

    digitalWrite(
        AC_PIN,
        on ? AC_ON : AC_OFF
    );

    saveACState();

    Serial.printf(
        "AC turned %s\n",
        on ? "ON" : "OFF"
    );
}


// ============================================================
// STATUS JSON
// ============================================================

String statusJson() {

    String json = "{";

    // Temperature
    if (isnan(lastTemp)) {

        json += "\"temperature_c\":null,";

    } else {

        json += "\"temperature_c\":" +
                String(lastTemp, 2) +
                ",";
    }


    // Humidity
    if (isnan(lastHum)) {

        json += "\"humidity_percent\":null,";

    } else {

        json += "\"humidity_percent\":" +
                String(lastHum, 1) +
                ",";
    }


    // Reading age
    unsigned long ageSeconds = 0;

    if (!isnan(lastTemp)) {

        ageSeconds =
            (millis() - lastGoodReadTime) / 1000;
    }

    json += "\"age_seconds\":" +
            String(ageSeconds) +
            ",";


    // Sensor statistics
    json += "\"fail_count\":" +
            String(failCount) +
            ",";

    json += "\"success_count\":" +
            String(successCount) +
            ",";


    // AC
    json += "\"ac\":" +
            String(acState ? "true" : "false") +
            ",";


    // Wi-Fi
    json += "\"wifi\":" +
            String(
                WiFi.status() == WL_CONNECTED
                ? "true"
                : "false"
            );

    json += "}";

    return json;
}


// ============================================================
// HTTP HANDLERS
// ============================================================


// ------------------------------------------------------------
// ROOT
// ------------------------------------------------------------

// ------------------------------------------------------------
// FREEZE PROTECTION (added 30 Oct 2026)
// A board can stay joined to Wi-Fi yet stop answering; it never sees a
// disconnect, so the reconnect logic above never runs. Two guards:
// - no HTTP request served for 5 min (the Pi polls every ~2 min): reboot;
// - hardware watchdog: loop() stuck for 30 s: the chip resets itself.
// ------------------------------------------------------------
unsigned long lastServed = 0;
const unsigned long SERVE_TIMEOUT = 5UL * 60UL * 1000UL;
const int HARDWARE_WATCHDOG_S = 30;

void touchServed() { lastServed = millis(); }

void checkServed() {
    if (millis() - lastServed > SERVE_TIMEOUT) {
        Serial.println("no HTTP request served in 5 min, rebooting");
        delay(100);
        ESP.restart();
    }
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

void handleRoot() {
    touchServed();

    server.send(
        200,
        "text/plain",

        "ESP32 SHT20 + AC controller\n"
        "\n"
        "GET /reading\n"
        "GET /status\n"
        "GET /ac?state=on\n"
        "GET /ac?state=off\n"
    );
}


// ------------------------------------------------------------
// /reading
// ------------------------------------------------------------

void handleReading() {
    touchServed();

    if (isnan(lastTemp) ||
        isnan(lastHum)) {

        server.send(
            503,
            "application/json",
            "{\"error\":\"No valid reading yet\"}"
        );

        return;
    }

    unsigned long ageSeconds =
        (millis() - lastGoodReadTime) / 1000;

    char payload[250];

    snprintf(
        payload,
        sizeof(payload),

        "{\"temperature_c\":%.2f,"
        "\"humidity_percent\":%.1f,"
        "\"age_seconds\":%lu,"
        "\"fail_count\":%d,"
        "\"success_count\":%d}",

        lastTemp,
        lastHum,
        ageSeconds,
        failCount,
        successCount
    );

    server.send(
        200,
        "application/json",
        payload
    );
}


// ------------------------------------------------------------
// /status
// ------------------------------------------------------------

void handleStatus() {
    touchServed();

    server.send(
        200,
        "application/json",
        statusJson()
    );
}


// ------------------------------------------------------------
// /ac
// ------------------------------------------------------------

void handleAC() {
    touchServed();

    if (!server.hasArg("state")) {

        server.send(
            400,
            "application/json",
            "{\"error\":\"missing state param\"}"
        );

        return;
    }

    String state = server.arg("state");

    state.toLowerCase();

    if (state != "on" &&
        state != "off") {

        server.send(
            400,
            "application/json",
            "{\"error\":\"state must be on or off\"}"
        );

        return;
    }

    setAC(state == "on");

    server.send(
        200,
        "application/json",
        statusJson()
    );
}


// ------------------------------------------------------------
// 404
// ------------------------------------------------------------

void handleNotFound() {

    server.send(
        404,
        "text/plain",
        "Not found"
    );
}


// ============================================================
// WIFI + MDNS
// ============================================================

void startMdns() {

    MDNS.end();

    if (MDNS.begin(HOSTNAME)) {

        MDNS.addService(
            "http",
            "tcp",
            80
        );

        Serial.printf(
            "mDNS responder started: "
            "http://%s.local\n",
            HOSTNAME
        );

    } else {

        Serial.println(
            "Error starting mDNS"
        );
    }
}


// ============================================================
// WIFI FAILSAFE / RECOVERY
// ============================================================

void ensureWifi() {

    unsigned long now = millis();

    if (now - lastWifiCheck <
        WIFI_CHECK_INTERVAL) {

        return;
    }

    lastWifiCheck = now;

    bool connected =
        WiFi.status() == WL_CONNECTED;


    // --------------------------------------------------------
    // WIFI IS CONNECTED
    // --------------------------------------------------------

    if (connected) {

        // Just recovered from a Wi-Fi failure
        if (!wifiUp) {

            Serial.println(
                "WiFi connection restored"
            );

            Serial.print(
                "IP address: "
            );

            Serial.println(
                WiFi.localIP()
            );

            // Reset failure timer
            wifiDisconnectedSince = 0;

            // Restart mDNS
            startMdns();
        }

        wifiUp = true;

        return;
    }


    // --------------------------------------------------------
    // WIFI IS DISCONNECTED
    // --------------------------------------------------------

    if (wifiUp) {

        Serial.println(
            "WiFi connection lost"
        );

        wifiUp = false;

        wifiDisconnectedSince = now;
    }


    // Start the timer if this is the first
    // disconnected check.
    if (wifiDisconnectedSince == 0) {

        wifiDisconnectedSince = now;

        Serial.println(
            "WiFi disconnected"
        );
    }


    unsigned long disconnectedFor =
        now - wifiDisconnectedSince;


    // --------------------------------------------------------
    // TRY NORMAL RECONNECT
    // --------------------------------------------------------

    Serial.printf(
        "WiFi reconnect attempt "
        "(disconnected for %lu seconds)\n",
        disconnectedFor / 1000
    );

    WiFi.reconnect();


    // --------------------------------------------------------
    // HARD RECOVERY
    // --------------------------------------------------------

    if (disconnectedFor >=
        WIFI_RECOVERY_TIMEOUT) {

        Serial.println();
        Serial.println(
            "================================"
        );
        Serial.println(
            "WiFi recovery timeout!"
        );
        Serial.println(
            "Resetting WiFi and restarting ESP32..."
        );
        Serial.println(
            "================================"
        );

        delay(100);

        // Disconnect WiFi completely
        WiFi.disconnect(true);

        delay(1000);

        // Full ESP restart
        ESP.restart();
    }
}


// ============================================================
// SETUP
// ============================================================

void setup() {

    Serial.begin(115200);

    delay(300);


    // --------------------------------------------------------
    // AC OUTPUT
    // --------------------------------------------------------

    // Set output latch to OFF before enabling output
    digitalWrite(
        AC_PIN,
        AC_OFF
    );

    pinMode(
        AC_PIN,
        OUTPUT
    );


    // --------------------------------------------------------
    // PREFERENCES
    // --------------------------------------------------------

    prefs.begin(
        "ac-control",
        false
    );

    acState =
        prefs.getBool(
            "ac",
            false
        );


    // Restore previous AC state
    digitalWrite(
        AC_PIN,
        acState ? AC_ON : AC_OFF
    );

    Serial.printf(
        "Restored AC state: %s\n",
        acState ? "ON" : "OFF"
    );


    // --------------------------------------------------------
    // SHT20 I2C
    // --------------------------------------------------------
    //
    // SDA = GPIO 21
    // SCL = GPIO 22
    //

    Wire.begin(
        21,
        22
    );

    Wire.setClock(
        100000
    );


    // --------------------------------------------------------
    // SHT20 PROBE
    // --------------------------------------------------------

    Wire.beginTransmission(
        SHT20_ADDR
    );

    if (Wire.endTransmission() == 0) {

        Serial.println(
            "SHT20 found on SDA=21 SCL=22"
        );

    } else {

        Serial.println(
            "SHT20 NOT found. "
            "Check wiring."
        );
    }


    // --------------------------------------------------------
    // WIFI
    // --------------------------------------------------------

    WiFi.mode(
        WIFI_STA
    );

    WiFi.setHostname(
        HOSTNAME
    );

    WiFi.setAutoReconnect(
        true
    );

    WiFi.persistent(
        false
    );


    // --------------------------------------------------------
    // DISABLE WIFI POWER SAVE
    // --------------------------------------------------------

    WiFi.setSleep(
        false
    );

    Serial.println(
        "WiFi sleep disabled"
    );


    // --------------------------------------------------------
    // REDUCE WIFI TX POWER
    // --------------------------------------------------------
    //
    // Lower transmit power reduces unnecessary RF power
    // when the ESP is reasonably close to the router.
    //

    WiFi.setTxPower(
        WIFI_POWER_13dBm
    );

    Serial.println(
        "WiFi TX power set to 13 dBm"
    );


    // --------------------------------------------------------
    // START WIFI
    // --------------------------------------------------------

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );


    Serial.print(
        "Connecting to WiFi"
    );

    int attempts = 0;

    while (
        WiFi.status() != WL_CONNECTED &&
        attempts < 40
    ) {

        delay(300);

        Serial.print(".");

        attempts++;
    }


    if (
        WiFi.status() != WL_CONNECTED
    ) {

        Serial.println(
            "\nWiFi FAILED. Status: " +
            String(WiFi.status())
        );

        // Start the Wi-Fi recovery timer.
        wifiUp = false;
        wifiDisconnectedSince = millis();

    } else {

        Serial.println();

        Serial.print(
            "IP address: "
        );

        Serial.println(
            WiFi.localIP()
        );

        wifiUp = true;
        wifiDisconnectedSince = 0;
    }


    // --------------------------------------------------------
    // MDNS
    // --------------------------------------------------------

    if (
        WiFi.status() == WL_CONNECTED
    ) {

        startMdns();
    }


    // --------------------------------------------------------
    // HTTP ROUTES
    // --------------------------------------------------------

    server.on(
        "/",
        handleRoot
    );

    server.on(
        "/reading",
        handleReading
    );

    server.on(
        "/status",
        handleStatus
    );

    server.on(
        "/ac",
        handleAC
    );

    server.onNotFound(
        handleNotFound
    );


    server.begin();

    Serial.println(
        "HTTP server started"
    );


    // --------------------------------------------------------
    // SENSOR INITIALIZATION
    // --------------------------------------------------------

    delay(2000);

    updateSensor();

    touchServed();
    startHardwareWatchdog();
}


// ============================================================
// LOOP
// ============================================================

void loop() {

    esp_task_wdt_reset();

    // Process HTTP requests
    server.handleClient();

    // Update SHT20
    updateSensor();

    // Monitor WiFi
    ensureWifi();

    // Frozen-but-connected guard
    checkServed();
}

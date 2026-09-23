#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include "../secrets.h"

#define INSIDE_HOSTEL

const char* HOSTNAME      = "stack1";
#ifdef INSIDE_HOSTEL
const char* WIFI_SSID     = WIFI_SSID_HOSTEL;
const char* WIFI_PASSWORD = WIFI_PASSWORD_HOSTEL;
#else
const char* WIFI_SSID     = WIFI_SSID_HOME;
const char* WIFI_PASSWORD = WIFI_PASSWORD_HOME;
#endif

ESP8266WebServer server(80);

void handleNotFound() {
  Serial.println("===== HTTP REQUEST =====");
  Serial.print("Method: ");
  Serial.println(server.method());
  Serial.print("URL: ");
  Serial.println(server.uri());
  Serial.println("Headers:");

  for (int i = 0; i < server.headers(); i++) {
    Serial.print("  ");
    Serial.print(server.headerName(i));
    Serial.print(": ");
    Serial.println(server.header(i));
  }

  Serial.println("========================");

  int attempts = 0;
  while (!Serial.available() && attempts < 100) { // wait at max 1 seconds
    server.handleClient();
    attempts++;
    delay(10);
  }

  if (attempts >= 500) {
    Serial.print("Sending response: ");
    Serial.println("404");
    server.send(404, "text/plain", "Not Found");
    return;
  }

  String response = Serial.readStringUntil('\n');
  response.trim();

  Serial.print("Sending response: ");
  Serial.println(response);

  server.send(200, "text/plain", response);
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

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

#ifndef INSIDE_HOSTEL
  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("mDNS responder started: http://%s.local\n", HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("Error starting mDNS");
  }
#endif

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
  digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
  server.handleClient();  
}
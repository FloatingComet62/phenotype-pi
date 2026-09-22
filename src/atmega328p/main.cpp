#include <Arduino.h>

// TODO: set it to the actual values
#define ACT_LED_PIN 0
#define ACT_MOTOR_PIN 1
#define SEN_PH 2
#define SEN_MOTOR_POWER_CONSUMPTION 3

void setup() {
    Serial.begin(115200);
}

String method;
String url;

// TODO: setLEd and setMotor will take params parsed from url
// so we need a function to parse params from url, and update the function
// signature to take input

// TODO: actally get the reading and trigger actuators

bool led_on = false;
void setLED() {
    led_on = !led_on;
}

bool motor_on = false;
void setMotor() {
    motor_on = !motor_on;
}

float last_reading_ph = -0.1;
float readPH() {
    return last_reading_ph;
}

float last_reading_motor_power_consumption = -0.1;
float readMotorPowerConsumption() {
    return last_reading_motor_power_consumption;
}

void loop() {
    if (!Serial.available()) return;

    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.startsWith("Method: ")) {
        method = line.substring(8);
    }

    if (line.startsWith("URL: ")) {
        url = line.substring(5);
    }

    if (line == "========================") {
        char opt[1024];
        sprintf(opt, "method: %s, url: %s\n", method.c_str(), url.c_str());

        if (url == "/led") {
            setLED();
            Serial.write(led_on ? "on" : "off");
        } else if (url == "/motor") {
            setMotor();
            Serial.write(motor_on ? "on" : "off");
        } else if (url == "/ph") {
            char reading[1024];
            snprintf(reading, 1024, "%.2f\n", readPH());
            Serial.write(reading);
        } else if (url == "/motor_power_consumption") {
            char reading[1024];
            snprintf(reading, 1024, "%.2f\n", readMotorPowerConsumption());
            Serial.write(reading);
        }
        //Serial.write(opt);
    }
}
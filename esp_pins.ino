//0byte@gvs
#include "config.h"

bool espPWMWrite(uint8_t pin, int duty) {
    duty = constrain(duty, 0, 255);
    pinMode(pin, OUTPUT);
    analogWrite(pin, duty);
    return true;
}

int espDigitalRead(uint8_t pin) {
    pinMode(pin, INPUT_PULLUP);
    return digitalRead(pin);
}

bool espDigitalWrite(uint8_t pin, int value) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, value ? HIGH : LOW);
    return true;
}

int espAnalogRead(uint8_t pin) {
    return analogRead(pin);
}

float espInternalTemp(void) {
    return 0.0f;
}

void espPinsInit(void) {
    Serial.println("[ESP32] Direct pin control ready");
    Serial.printf("[ESP32] Fan on GPIO%d (PWM via analogWrite)\n", FAN_PIN);
}
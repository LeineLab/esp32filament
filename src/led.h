#pragma once
#include <Arduino.h>
#include "config.h"

// On-board RGB LED (CYD) — common anode: LOW = on, HIGH = off.
// PWM brightness: 0 = off, 255 = full brightness.

inline void led_init() {
    pinMode(LED_R, OUTPUT);
    pinMode(LED_G, OUTPUT);
    pinMode(LED_B, OUTPUT);
    digitalWrite(LED_R, HIGH);
    digitalWrite(LED_G, HIGH);
    digitalWrite(LED_B, HIGH);
}

// Set RGB with 0–255 brightness each (common anode: value is inverted internally)
inline void led_set(uint8_t r, uint8_t g, uint8_t b) {
    analogWrite(LED_R, 255 - r);
    analogWrite(LED_G, 255 - g);
    analogWrite(LED_B, 255 - b);
}

inline void led_off() {
    // Use analogWrite (LEDC) consistently — once any led_* call has switched a
    // pin to LEDC mode, digitalWrite() on it fails with "not set as GPIO".
    analogWrite(LED_R, 255);
    analogWrite(LED_G, 255);
    analogWrite(LED_B, 255);
}

// Convenience colours
inline void led_red()    { led_set(200,   0,   0); }
inline void led_green()  { led_set(  0, 200,   0); }
inline void led_blue()   { led_set(  0,   0, 200); }
inline void led_orange() { led_set(200, 120,   0); }
inline void led_white()  { led_set(150, 150, 150); }

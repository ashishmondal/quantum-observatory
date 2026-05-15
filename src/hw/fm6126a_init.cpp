#include "fm6126a_init.h"

#include <Arduino.h>

#include "config.h"

namespace fm6126a {

void init() {
  const uint8_t rgb[6]  = {PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2};
  const uint8_t addr[5] = {PIN_A,  PIN_B,  PIN_C,  PIN_D,  PIN_E};

  for (uint8_t p : rgb)  { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  for (uint8_t p : addr) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  pinMode(PIN_CLK, OUTPUT); digitalWrite(PIN_CLK, LOW);
  pinMode(PIN_STB, OUTPUT); digitalWrite(PIN_STB, LOW);
  pinMode(PIN_OE,  OUTPUT); digitalWrite(PIN_OE,  HIGH);  // blank

  const int MaxLed = 64;
  const int C12[16] = {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
  const int C13[16] = {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0};

  // Register 12
  for (int l = 0; l < MaxLed; l++) {
    int y = l % 16;
    int v = C12[y] ? HIGH : LOW;
    for (uint8_t p : rgb) digitalWrite(p, v);
    digitalWrite(PIN_STB, (l > MaxLed - 12) ? HIGH : LOW);
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_CLK, LOW);
  }
  digitalWrite(PIN_STB, LOW);
  digitalWrite(PIN_CLK, LOW);

  // Register 13
  for (int l = 0; l < MaxLed; l++) {
    int y = l % 16;
    int v = C13[y] ? HIGH : LOW;
    for (uint8_t p : rgb) digitalWrite(p, v);
    digitalWrite(PIN_STB, (l > MaxLed - 13) ? HIGH : LOW);
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_CLK, LOW);
  }
  digitalWrite(PIN_STB, LOW);
  digitalWrite(PIN_CLK, LOW);
}

}  // namespace fm6126a

// Implementation of include/light_sensor.h. earlephilhower's analogRead()
// returns 10-bit by default; we pull the raw 12-bit value for headroom
// (matches the vendor reference). Threshold check uses Schmitt-style
// hysteresis: drop below `threshold` to enter night, rise above
// `threshold + hysteresis` to leave it. Symmetric latching avoids
// flicker at the boundary.

#include "light_sensor.h"

#include <Arduino.h>
#include <hardware/adc.h>

#include "config.h"

namespace light_sensor {

namespace {

bool     s_inited        = false;
uint32_t s_last_poll_ms  = 0;
uint16_t s_raw           = 0;
bool     s_night         = false;

// Mutable so 5.5.3 can swap them in over MQTT without reaching into
// the namespace internals.
uint16_t s_threshold     = LIGHT_NIGHT_THRESHOLD_DEFAULT;
uint16_t s_hysteresis    = LIGHT_NIGHT_HYSTERESIS_DEFAULT;

constexpr uint32_t kPollIntervalMs = 1000u;  // FR-7.1 "≥ 1 Hz"

}  // namespace

void begin() {
  if (s_inited) return;
  // earlephilhower core wraps the SDK adc_* APIs; we use them directly
  // so we get a 12-bit raw read without the analogRead() default scale.
  adc_init();
  adc_gpio_init(PIN_LIGHT_SENSOR);
  s_inited = true;
  Serial.print("[light] adc gp=");
  Serial.print(PIN_LIGHT_SENSOR);
  Serial.print(" night_threshold=");
  Serial.print(s_threshold);
  Serial.print(" hysteresis=");
  Serial.println(s_hysteresis);
}

bool poll(uint32_t now_ms) {
  if (!s_inited) return false;
  if (now_ms - s_last_poll_ms < kPollIntervalMs) return false;
  s_last_poll_ms = now_ms;

  // ADC channel selection is shared state across the chip; re-select
  // each poll in case some other module (future temp-via-internal-sensor
  // etc.) reaches into it. Cheap.
  adc_select_input(LIGHT_SENSOR_ADC_INPUT);
  s_raw = adc_read();  // 0..4095

  const bool was_night = s_night;
  // On this board HIGHER raw = DARKER (see config.h). Hysteresis:
  // enter night at/above threshold; leave only when comfortably below
  // threshold - hysteresis.
  if (s_night) {
    if (s_raw + s_hysteresis < s_threshold) {
      s_night = false;
    }
  } else {
    if (s_raw >= s_threshold) {
      s_night = true;
    }
  }

  if (s_night != was_night) {
    Serial.print("[light] night=");
    Serial.print(s_night ? 1 : 0);
    Serial.print(" raw=");
    Serial.println(s_raw);
    return true;
  }
  return false;
}

uint16_t raw()      { return s_raw; }
bool     is_night() { return s_night; }

void set_thresholds(uint16_t threshold, uint16_t hysteresis) {
  // Single-core write (MQTT callback runs on Core 0, same as poll())
  // — no mutex needed. Plain assignment is atomic for uint16_t on
  // RP2040 (naturally aligned 16-bit store).
  s_threshold  = threshold;
  s_hysteresis = hysteresis;
  Serial.print("[light] thresholds threshold=");
  Serial.print(s_threshold);
  Serial.print(" hysteresis=");
  Serial.println(s_hysteresis);
}

}  // namespace light_sensor

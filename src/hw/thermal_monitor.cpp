// Implementation of include/thermal_monitor.h. Mirrors the
// light_sensor.cpp shape (Schmitt-style hysteresis, transition-only
// log line) so the two FR-7 sensors look the same to future readers.

#include "thermal_monitor.h"

#include <Arduino.h>
#include <stdint.h>

#include "config.h"
#include "ds3231.h"

namespace thermal_monitor {

namespace {

bool     s_inited       = false;
uint32_t s_last_poll_ms = 0;
int8_t   s_temp_c       = INT8_MIN;
bool     s_hot          = false;

// Mutable so 5.5.3 can swap them in over MQTT without reaching into
// the namespace internals.
int8_t s_threshold  = THERMAL_THRESHOLD_C_DEFAULT;
int8_t s_hysteresis = THERMAL_HYSTERESIS_C_DEFAULT;

// 10 s is plenty: the DS3231 only auto-converts every 64 s, so faster
// polling would just churn I²C. Comfortably above the FR-7 / NFR-4.1
// "≥ 0.1 Hz" floor.
constexpr uint32_t kPollIntervalMs = 10000u;

}  // namespace

void begin() {
  if (s_inited) return;
  s_inited = true;
  Serial.print("[thermal] mon threshold_c=");
  Serial.print(static_cast<int>(s_threshold));
  Serial.print(" hysteresis_c=");
  Serial.println(static_cast<int>(s_hysteresis));
}

bool poll(uint32_t now_ms) {
  if (!s_inited) return false;
  if (now_ms - s_last_poll_ms < kPollIntervalMs) return false;
  s_last_poll_ms = now_ms;

  int8_t t;
  if (!ds3231::read_temp_c(&t)) {
    // Treat a failed read as "no new information" — keep last state,
    // don't trip into thermal_safe on a one-off I²C glitch.
    return false;
  }
  s_temp_c = t;

  const bool was_hot = s_hot;
  // Schmitt: enter hot at/above threshold; leave only when comfortably
  // below threshold - hysteresis. Mirrors light_sensor's polarity-
  // adjusted version. (Use plain int math to dodge any int8 overflow
  // around the boundaries.)
  const int temp_i = static_cast<int>(s_temp_c);
  const int thr    = static_cast<int>(s_threshold);
  const int hys    = static_cast<int>(s_hysteresis);
  if (s_hot) {
    if (temp_i + hys < thr) {
      s_hot = false;
    }
  } else {
    if (temp_i >= thr) {
      s_hot = true;
    }
  }

  if (s_hot != was_hot) {
    Serial.print("[thermal] hot=");
    Serial.print(s_hot ? 1 : 0);
    Serial.print(" c=");
    Serial.println(static_cast<int>(s_temp_c));
    return true;
  }
  return false;
}

int8_t last_temp_c() { return s_temp_c; }
bool   is_hot()      { return s_hot; }

void set_thresholds(int8_t threshold_c, int8_t hysteresis_c) {
  // Single-core write (MQTT callback runs on Core 0). int8 stores are
  // trivially atomic on RP2040.
  s_threshold  = threshold_c;
  s_hysteresis = hysteresis_c;
  Serial.print("[thermal] thresholds threshold_c=");
  Serial.print(static_cast<int>(s_threshold));
  Serial.print(" hysteresis_c=");
  Serial.println(static_cast<int>(s_hysteresis));
}

}  // namespace thermal_monitor

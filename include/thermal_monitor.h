// DS3231 on-die temperature monitor — drives the FR-7.3 thermal-safe
// override. Polls reg 0x11 from Core 0 (cheap I²C transaction, chip
// auto-converts every 64 s so faster polling buys nothing) with
// hysteresis around the configured threshold to avoid edge flicker.
//
// Same shape as light_sensor: begin() + rate-limited poll() returning
// true on a debounced transition, plus accessors. Threshold +
// hysteresis defaults come from config.h; 5.5.3 will allow MQTT to
// overwrite them at runtime.
//
// Important caveat: this is the DS3231 silicon temperature, NOT the
// HUB75 panel surface (Open Question §9.8). The default trip point is
// a conservative placeholder until we soak-test the offset.
//
// (added in phase 5.5.2)

#pragma once

#include <stdint.h>

namespace thermal_monitor {

// One-time bring-up. ds3231::begin() must have been called first
// (we share Wire1). Idempotent.
void begin();

// Sample-and-decide. Call from loop() at any cadence; the function is
// internally rate-limited (≥ 0.1 Hz per FR-7 / NFR-4.1). Returns true
// iff the hot-active state changed on this call. On a transition, the
// new value is reflected in is_hot().
bool poll(uint32_t now_ms);

// Latest successfully-read temperature (°C, signed). INT8_MIN before
// the first successful poll. Useful for diagnostics and for the
// thermal_safe scene to display the current value.
int8_t last_temp_c();

// Latest debounced thermal-safe decision.
bool is_hot();

// Live threshold update (FR-7.4). Called from the Core 0 MQTT
// callback when `observatory/thermal` arrives with a valid
// `{threshold,hysteresis}` payload. Both values are degrees Celsius
// (signed). The next poll() resolves with the new values; the hot
// state is NOT recomputed here, by design. (added in phase 5.5.3)
void set_thresholds(int8_t threshold_c, int8_t hysteresis_c);

}  // namespace thermal_monitor

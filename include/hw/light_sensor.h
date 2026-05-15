// Photoresistor / ambient-light sensor on ADC0 (GP26) — drives the FR-7.2
// night-mode swap. Polled at ~1 Hz from Core 0 (cheap), with hysteresis
// around the configured threshold to avoid flicker at the edge.
//
// Pure read-only on the hardware side: ADC init in begin(), one
// adc_read() per poll(). The "is it night?" decision lives here; the
// cross-core flag publish (scene_state::set_night_active) is the
// caller's job — keeps this module unit-testable in isolation.
//
// Threshold + hysteresis defaults come from config.h. 5.5.3 will allow
// MQTT to overwrite them at runtime via setters.
//
// (added in phase 5.5.1)

#pragma once

#include <stdint.h>

namespace light_sensor {

// One-time ADC bring-up. Idempotent. Call from setup() on Core 0.
void begin();

// Sample-and-decide. Call at ~1 Hz from loop() (cheaper polls are
// fine; the hysteresis state machine doesn't care about cadence).
// Returns true iff the night-active state changed on this call.
// On a transition, the new value is reflected in is_night().
bool poll(uint32_t now_ms);

// Latest debounced raw 12-bit reading from the most recent poll(). 0
// before the first successful poll. Useful for diagnostics / threshold
// calibration (Open Question §9.7).
uint16_t raw();

// Latest debounced night decision.
bool is_night();

// Live threshold update (FR-7.4). Called from the Core 0 MQTT
// callback when `observatory/night` arrives with a valid
// `{threshold,hysteresis}` payload. Both values are 12-bit ADC units
// (0..4095). Applied atomically from the caller's perspective: the
// next poll() uses the new values. The decision state (s_night) is
// intentionally NOT recomputed here — the next poll() will resolve
// it normally with hysteresis, which is the right behaviour at the
// boundary. (added in phase 5.5.3)
void set_thresholds(uint16_t threshold, uint16_t hysteresis);

}  // namespace light_sensor

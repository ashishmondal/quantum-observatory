// observatory/moon — Director-pushed lunar state.
//
// HA can publish richer / higher-precision moon info than the firmware
// can compute locally (e.g. astronomical libraries with proper
// libration corrections, phase names matching its own UI). When that
// data is fresh, MoonPhaseScene reads from this snapshot; otherwise
// it falls back to its on-board synodic-month calculation.
//
// Cross-core: writer is Core 0 (MQTT callback), reader is Core 1
// (render). Multi-field state → real mutex_t per CODING_PRACTICES §3.
//
// Freshness window: kFreshMs is generous — moon position drifts at
// ~0.5°/h, far slower than HA could go down without us noticing. A
// 12 h ceiling means a missed update over a Wi-Fi outage doesn't
// blank the readout, while still tripping the fallback if HA goes
// dark for a full day.
//
// (added in phase 7.2 follow-up — MQTT data path for moon scene)

#pragma once

#include <stdint.h>

namespace moon_state {

struct Snapshot {
  bool        valid;        // false until first set_from_mqtt OR stale
  float       phase_frac;   // 0..1 (0 = new, 0.5 = full)
  uint8_t     illum_pct;    // 0..100
  uint16_t    age_d;        // 0..29
  char        name[12];     // NUL-terminated, "FULL" / "WAX GIB" / …
};

constexpr uint32_t kFreshMs = 12u * 60u * 60u * 1000u;  // 12 h

// One-time mutex init. Call from setup() before either core spins.
void init();

// MQTT writer. Validation happens in mqtt_link before this is called;
// values here are assumed in-range. `name` may be nullptr/empty — the
// scene will derive it from phase_frac if so.
void set_from_mqtt(float phase_frac, uint8_t illum_pct, uint16_t age_d,
                   const char* name, uint32_t now_ms);

// Reader. Returns true with a fresh snapshot if a value has been
// pushed in the last kFreshMs and was actually set; false otherwise
// (caller should fall back to local calculation).
bool get(uint32_t now_ms, Snapshot* out);

}  // namespace moon_state

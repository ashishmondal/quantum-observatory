// observatory/planet — generic per-body live-overlay data path for
// the `planets` scene.
//
// Replaces the body-specific observatory/jupiter topic. HA picks
// which body to push (typically one of sun / moon / planets that
// skyfield can ephemeris) and the scene overlays the look-angles
// on line 3 ONLY when the active body matches `name` (case-
// insensitive). Bodies not pushed by HA stay on the static fact
// string from planet_catalog.h.
//
// Wire fields (see docs/MQTT_TOPICS.md):
//   name                — required, ASCII string ≤ kNameCap-1 chars,
//                          matched case-insensitively against
//                          planet_catalog::kBodies[*].render_name
//                          and ProceduralPlanet preset keys
//   bearing_deg         — required, 0..359 compass azimuth
//   elevation_deg       — required, -90..+90 altitude (negative = below)
//   constellation_index — required, 0..87 (same encoding as
//                          observatory/constellation), drives the
//                          daylight "IN <IAU>" fallback if/when the
//                          scene later wants it.
//
// Cross-core: writer = Core 0 (MQTT callback), reader = Core 1
// (render). Multi-field snapshot → real mutex_t per
// CODING_PRACTICES §3.
//
// Freshness ceiling: 1 h (planetary look-angles drift slowly).
//
// (added when jupiter_visibility was generalised into the `planets`
//  scene; mirrors the iss_state / moon_state IPC pattern.)

#pragma once

#include <stdint.h>

namespace planet_state {

// Max name length on the wire (NUL-terminated). Sized to fit every
// kBodies[*].render_name plus a couple of bytes of headroom for HA-
// side typo'd payloads we still want to capture for the logs.
constexpr uint8_t kNameCap = 16;

struct Snapshot {
  bool     valid;                  // false until first set_from_mqtt OR stale
  char     name[kNameCap];         // NUL-terminated; empty when !valid
  int16_t  bearing_deg;            // 0..359
  int8_t   elevation_deg;          // -90..+90
  uint8_t  constellation_index;    // 0..87
  uint32_t set_at_ms;
};

constexpr uint32_t kFreshMs = 60u * 60u * 1000u;  // 1 h

// One-time mutex init. Call from setup() before either core spins.
void init();

// MQTT writer. Validation + range-clamping happens in mqtt_link
// before this is called; values here are assumed in-range and
// `name` is assumed to be a NUL-terminated string ≤ kNameCap-1 chars.
void set_from_mqtt(const char* name,
                   int16_t bearing_deg, int8_t elevation_deg,
                   uint8_t constellation_index,
                   uint32_t now_ms);

// Reader. Returns true with a fresh snapshot if a value has been
// pushed in the last kFreshMs; false otherwise.
bool get(uint32_t now_ms, Snapshot* out);

}  // namespace planet_state

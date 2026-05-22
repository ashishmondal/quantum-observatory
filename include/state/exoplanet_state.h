// observatory/exoplanet — raw HA pass-through of NASA Exoplanet Archive
// stats for the `exoplanet_count` scene (phase 7.7).
//
// Same Director/Cinematographer split as iss / jupiter (phase 7.1++):
// HA's job is a daily REST poll against the NASA Exoplanet Archive
// TAP endpoint + Jinja remap — no observer-frame logic, no "is it
// interesting" booleans. The firmware decides freshness, renders the
// procedural planet for `nearest_name`, and types the three data
// lines.
//
// Wire fields:
//   total_count             — required, 0..2_000_000, current count of
//                              confirmed exoplanets in the archive.
//   added_recent            — optional, -10000..+10000, signed delta
//                              over an HA-pinned rolling window
//                              (currently 7 d). Absent → renders as
//                              "+0 WK". Signed because the archive
//                              occasionally revises down on
//                              re-classification.
//   nearest_name            — required, 1..23 ASCII chars, name of
//                              the nearest known exoplanet (e.g.
//                              "Proxima b"). Seeds the procedural
//                              planet rendering.
//   nearest_distance_ly     — optional, 0..6553.5 light-years, stored
//                              ×10 as Q4.1 fixed-point. Absent →
//                              renders as "NR ? LY".
//
// Cross-core: writer is Core 0 (MQTT callback), reader is Core 1
// (render). Multi-field snapshot → real mutex_t per
// CODING_PRACTICES §3.
//
// Freshness: 48 h ceiling (`kFreshMs`). The archive is updated daily
// at most and the "nearest known" rarely changes year-to-year, so a
// 48 h window survives a missed publish without blanking the scene
// for the kid; past that the scene falls back to "WAIT" rather than
// fabricating data.
//
// (added in phase 7.7)

#pragma once

#include <stdint.h>

namespace exoplanet_state {

constexpr uint8_t  kNameCap   = 24;                    // 23 chars + NUL
constexpr uint32_t kFreshMs   = 48u * 60u * 60u * 1000u;  // 48 h

struct Snapshot {
  bool     valid;                       // false until first set OR stale

  uint32_t total_count;                 // 0..2_000_000
  bool     have_added_recent;
  int16_t  added_recent;                // -10000..+10000 when present

  char     nearest_name[kNameCap];      // NUL-terminated, ASCII
  bool     have_nearest_distance;
  uint16_t nearest_distance_ly_x10;     // 0..65535 (Q4.1)

  uint32_t set_at_ms;                   // millis() when push applied
};

// One-time mutex init. Call from setup() before either core spins.
void init();

// MQTT writer. Validation + range clamping happens in mqtt_link
// before this is called; values here are assumed in-range. The name
// is copied under the mutex with explicit length capping; pass any
// NUL-terminated string up to (kNameCap - 1) chars.
//
// `have_added_recent` and `have_nearest_distance` mark optional fields
// as present/absent — pass false to render placeholders on screen.
void set_from_mqtt(uint32_t total_count,
                   bool have_added_recent, int16_t added_recent,
                   const char* nearest_name,
                   bool have_nearest_distance,
                   uint16_t nearest_distance_ly_x10,
                   uint32_t now_ms);

// Reader. Returns true with a fresh snapshot if a value has been
// pushed in the last kFreshMs; false otherwise (caller renders a
// "WAIT" placeholder).
bool get(uint32_t now_ms, Snapshot* out);

}  // namespace exoplanet_state

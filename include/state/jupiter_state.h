// observatory/jupiter — raw HA pass-through of Jupiter's apparent
// position + brightness for the `jupiter_visibility` scene.
//
// Same Director/Cinematographer split as iss_state (phase 7.1++):
// HA's job is a single REST poll + Jinja remap — no observer-frame
// logic, no "is it visible from MY backyard" booleans. The firmware
// computes visibility every frame from this snapshot + the observer
// lat/lon in config.h + sun::compute(). HA picks any astronomy
// integration that exposes `azimuth` + `altitude` for Jupiter (e.g.
// the community ephemeris/astroweather components built on
// pyephem/skyfield) and re-emits them verbatim.
//
// Wire fields:
//   bearing_deg         — required, 0..359, compass azimuth (0=N, 90=E)
//   elevation_deg       — required, -90..90, altitude above horizon
//                          (negative = below horizon)
//   magnitude           — optional, ~-3..+1 in practice, capped -30..30
//   distance_au         — optional, ~4..6 AU in practice, capped 0..100
//   constellation_index — required, 0..87, index into
//                          `constellations_iau::kCatalog[]` (same
//                          encoding as observatory/constellation).
//                          Drives the scene's line-3 daylight
//                          readout `IN <IAU>` (e.g. `IN TAU`) when
//                          Jupiter is above the horizon but the
//                          observer isn't dark enough yet.
//
// Unlike ISS, Jupiter is *always* sunlit (planets shine by reflected
// light), so there is no `sunlit` field — only the observer-side
// darkness condition matters for naked-eye visibility. The scene
// decides:
//   visible = (elevation_deg >= 0)            // above the horizon
//          && (sun::compute().alt <= -6°)     // observer in twilight
//
// Cross-core: writer is Core 0 (MQTT callback), reader is Core 1
// (render). Multi-field snapshot → real mutex_t per
// CODING_PRACTICES §3.
//
// Freshness: 1 h ceiling (`kFreshMs`). Jupiter's apparent position
// drifts ~0.5°/h max so even an hour-stale snapshot still points the
// kid at roughly the right patch of sky; past that the scene blanks
// to "WAIT" rather than fabricating data.
//
// (added in phase 7.3)

#pragma once

#include <stdint.h>

namespace jupiter_state {

struct Snapshot {
  bool     valid;            // false until first set_from_mqtt OR stale

  // Required look-angles. HA pushes them straight through; the scene
  // never recomputes them, only consumes them.
  int16_t  bearing_deg;      // 0..359 compass azimuth
  int8_t   elevation_deg;    // -90..90; negative = below horizon

  // Optional — render "?" when absent on the wire. magnitude is
  // stored ×10 (Q4.1 fixed-point) so a 1-decimal readout costs no
  // float math in the render loop (CODING_PRACTICES §2 / NFR-1.3).
  bool     have_magnitude;
  int16_t  magnitude_x10;    // -300..300 (i.e. -30.0..+30.0)
  bool     have_distance;
  uint16_t distance_au_x10;  // 0..1000   (i.e.   0.0..100.0)
  uint8_t  constellation_index;  // 0..87, index into kCatalog[]

  uint32_t set_at_ms;        // millis() when the push was applied
};

constexpr uint32_t kFreshMs = 60u * 60u * 1000u;  // 1 h

// One-time mutex init. Call from setup() before either core spins.
void init();

// MQTT writer. Validation + range-clamping happens in mqtt_link
// before this is called; values here are assumed in-range. Pass
// have_magnitude=false / have_distance=false to leave the optional
// fields as "?" on screen.
void set_from_mqtt(int16_t bearing_deg, int8_t elevation_deg,
                   bool have_magnitude, int16_t magnitude_x10,
                   bool have_distance, uint16_t distance_au_x10,
                   uint8_t constellation_index,
                   uint32_t now_ms);

// Reader. Returns true with a fresh snapshot if a value has been
// pushed in the last kFreshMs; false otherwise (caller renders a
// "WAIT" placeholder).
bool get(uint32_t now_ms, Snapshot* out);

}  // namespace jupiter_state

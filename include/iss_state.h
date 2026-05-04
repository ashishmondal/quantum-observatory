// observatory/iss — raw HA pass-through of the public ISS data feeds.
//
// Design contract: HA does **no logic**. Its job is to poll a couple
// of no-auth REST sources (wheretheiss.at for live position +
// sunlight, open-notify for crew + next-pass) and re-emit the fields
// verbatim through a Jinja template. All "is the station visible
// from MY backyard right now / which way do I point" geometry is
// computed on-device every frame from this snapshot + the observer
// lat/lon baked into config.h. That keeps the wire contract trivially
// fuzzable and matches CODING_PRACTICES — the Director observes the
// world, the Cinematographer renders.
//
// Fields landing here are just whatever the upstream APIs returned,
// rounded to ints/floats the firmware likes. Derived quantities
// (visible boolean, look-angles for the "VIS BBBxEE" readout) are
// produced by the iss_pass scene from `sun::compute()` +
// `iss_geom::look_angles()`, never by HA.
//
// Cross-core: writer is Core 0 (MQTT callback), reader is Core 1
// (render). Multi-field state → real mutex_t per CODING_PRACTICES §3.
//
// Freshness window: 1 h ceiling. wheretheiss is fine at 30 s polls;
// crew_count changes weekly so HA may publish it less often. The
// scene blanks to "WAIT" rather than fabricating data once stale.
//
// `seconds_until_next` is captured at `set_at_ms`; the scene projects
// the live remaining count via `(now_ms - set_at_ms) / 1000` so the
// panel ticks down second-by-second between MQTT pushes.
//
// (added in phase 7.1+; reshaped in 7.1++ to match raw HA pass-through)

#pragma once

#include <stdint.h>

namespace iss_state {

struct Snapshot {
  bool     valid;               // false until first set_from_mqtt OR stale

  // Live sub-satellite point + orbital state, straight from
  // wheretheiss.at /v1/satellites/25544. Required on every push.
  float    iss_lat_deg;         // -90..+90, +N / -S
  float    iss_lon_deg;         // -180..+180, +E / -W
  uint16_t altitude_km;         // 0..999, orbital altitude in km
  bool     sunlit;              // true = ISS in sunlight (NOT
                                // observer-visibility — that's derived)

  // Next-pass countdown anchor, from open-notify iss-pass.json.
  // Required. Scene projects the live tick-down via millis() delta.
  uint32_t seconds_until_next;  // 0..604800 (≤ 1 week)

  // People in space (open-notify astros.json filtered to ISS).
  // Optional — changes weekly, HA may not have polled yet on boot.
  bool     have_crew;
  uint8_t  crew_count;          // 0..99

  uint32_t set_at_ms;           // millis() when the push was applied
};

constexpr uint32_t kFreshMs = 60u * 60u * 1000u;  // 1 h

// One-time mutex init. Call from setup() before either core spins.
void init();

// MQTT writer. Validation happens in mqtt_link before this is called;
// values here are assumed in-range. `crew_count` is the only
// optional field — pass have_crew=false to leave it as "?" on screen.
void set_from_mqtt(float iss_lat_deg, float iss_lon_deg,
                   uint16_t altitude_km, bool sunlit,
                   uint32_t seconds_until_next,
                   bool have_crew, uint8_t crew_count,
                   uint32_t now_ms);

// Reader. Returns true with a fresh snapshot if a value has been
// pushed in the last kFreshMs; false otherwise (caller renders a
// "WAIT" placeholder).
bool get(uint32_t now_ms, Snapshot* out);

}  // namespace iss_state

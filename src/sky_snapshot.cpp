// Implementation of include/sky_snapshot.h. See header for the
// cross-core model and the FR-16.5 / FR-16.9 contract.

#include "sky_snapshot.h"

#include <math.h>
#include <stdint.h>

#include <Arduino.h>  // pulls pico.h transitively — required before seq_snapshot.h

#include "config.h"
#include "iss_geometry.h"
#include "iss_state.h"
#include "seq_snapshot.h"
#include "sun_position.h"
#include "time_of_day.h"

namespace sky_snapshot {

namespace {

// 1 Hz refresh cadence — sun moves ~0.25°/s, way slower than a panel
// pixel; moon phase changes ~3e-7 per second. Anything faster would
// be wasted Core-1 budget for zero visible benefit.
constexpr uint32_t kRefreshIntervalMs = 1000;

// Closed-form synodic-month constants. Match the moon_phase scene's
// fallback path so a snapshot reader and a scene-local fallback
// produce identical numbers.
constexpr int32_t kNewMoonRefEpoch = 947182440;   // 2000-01-06 18:14 UTC
constexpr int32_t kSynodicSec      = 2551443;     // 29.530588 d

SeqSnapshot<Snapshot> s_pub;
uint32_t              s_version = 0;
uint32_t              s_next_refresh_ms = 0;

// Compute moon phase + illum + age from the local epoch. Returns
// numbers identical to MoonPhaseScene's fallback path.
void compute_moon(int32_t local_epoch, float* phase_frac,
                  uint8_t* illum_pct, uint8_t* age_d) {
  int32_t age_sec = (local_epoch - kNewMoonRefEpoch) % kSynodicSec;
  if (age_sec < 0) age_sec += kSynodicSec;
  const float p = static_cast<float>(age_sec)
                / static_cast<float>(kSynodicSec);
  *phase_frac = p;
  *age_d      = static_cast<uint8_t>(age_sec / 86400);
  // Illumination: (1 - cos(2π·phase)) / 2.
  const float cos_p = cosf(6.2831853f * p);
  int illum = static_cast<int>((1.0f - cos_p) * 50.0f + 0.5f);
  if (illum < 0)   illum = 0;
  if (illum > 100) illum = 100;
  *illum_pct = static_cast<uint8_t>(illum);
}

}  // namespace

void init() {
  // Seed an explicitly-invalid snapshot so first-frame readers see
  // valid=false rather than uninitialised junk. version=0 is fine —
  // readers either check `valid` or compare versions monotonically.
  Snapshot snap{};
  snap.version = 0;
  snap.valid   = false;
  s_pub.publish(snap);
  s_next_refresh_ms = 0;
}

bool tick(uint32_t now_ms, uint32_t slack_ms) {
  // FR-16.9: skip when Core 1 is already over budget. The floor is
  // intentionally generous (~20 % of kFrameIntervalMs) so a heavy
  // scene can spend the full frame on itself without sky-model work
  // piling on. Off-budget scenes will simply observe a stale
  // snapshot; sun azimuth drifts ~0.25°/s so a multi-second stall
  // is invisible at panel resolution.
  if (slack_ms < static_cast<uint32_t>(RENDER_SLACK_FLOOR_MS_DEFAULT)) {
    return false;
  }

  // Wrap-safe deadline check (CODING_PRACTICES §2). On the very
  // first call s_next_refresh_ms == 0 so the comparison is
  // unconditionally true and we run immediately.
  if (static_cast<int32_t>(now_ms - s_next_refresh_ms) < 0) {
    return false;
  }
  s_next_refresh_ms = now_ms + kRefreshIntervalMs;

  // FR-9.6: no time → no sky model. Publish an explicitly-invalid
  // snapshot so readers can distinguish "RTC dead" from "Core 1
  // hasn't ticked yet" (the init() seed also sets valid=false but
  // version=0; here version advances).
  const tod::Reading r = tod::now(now_ms);
  if (!r.valid) {
    Snapshot snap{};
    snap.version        = ++s_version;
    snap.valid          = false;
    snap.computed_at_ms = now_ms;
    s_pub.publish(snap);
    return true;
  }

  // RTC stores LOCAL time per the Phase 3.6.3 decision; convert to
  // UTC for the astronomy math.
  const int32_t utc_epoch = r.local_epoch
      - static_cast<int32_t>(LOCAL_TZ_OFFSET_MIN) * 60;

  Snapshot snap{};
  snap.version = ++s_version;
  snap.valid   = true;

  // ── Sun ────────────────────────────────────────────────────────
  const sun::Position sp =
      sun::compute(utc_epoch, LATITUDE_DEG, LONGITUDE_DEG);
  snap.sun_altitude_deg = sp.altitude_deg;
  snap.sun_azimuth_deg  = sp.azimuth_deg;
  snap.observer_dark    = (sp.altitude_deg <= -6.0f);

  // ── Moon ───────────────────────────────────────────────────────
  compute_moon(r.local_epoch, &snap.moon_phase_frac,
               &snap.moon_illum_pct, &snap.moon_age_d);

  // ── ISS ────────────────────────────────────────────────────────
  // Only populated when the iss_state cache is fresh (HA has pushed
  // within iss_state::kFreshMs). Otherwise readers see
  // iss_have_position=false and fall back to "WAIT".
  iss_state::Snapshot iss;
  if (iss_state::get(now_ms, &iss)) {
    const iss_geom::LookAngles la = iss_geom::look_angles(
        LATITUDE_DEG, LONGITUDE_DEG,
        iss.iss_lat_deg, iss.iss_lon_deg,
        static_cast<float>(iss.altitude_km));
    snap.iss_have_position = true;
    snap.iss_bearing_deg   = la.azimuth_deg;
    snap.iss_elevation_deg = la.elevation_deg;
    snap.iss_visible = iss.sunlit
                    && snap.observer_dark
                    && (la.elevation_deg >= 0.0f);
  } else {
    snap.iss_have_position = false;
    snap.iss_visible       = false;
  }

  snap.computed_at_ms = now_ms;
  s_pub.publish(snap);
  return true;
}

uint32_t read(Snapshot* out) {
  return s_pub.read(out);
}

}  // namespace sky_snapshot

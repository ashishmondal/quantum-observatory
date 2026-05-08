// Continuous sky-model snapshot — Core 1 background simulation.
//
// FR-16.5: a 1 Hz sky simulation runs on Core 1 between frames during
// the slack window (FR-16.9), regardless of which scene is active.
// Computes:
//   - Sun altitude/azimuth (sun::compute against the live RTC).
//   - Moon phase fraction + illumination (closed-form synodic-month
//     calc; ≤ ±2 % phase error per FR-14.4).
//   - ISS observer-frame look-angles + visibility (when iss_state is
//     fresh — falls back to "no position" when HA hasn't pushed).
//
// Results are published into a shared snapshot read by any scene and
// by the chrome layer. Two wins:
//   (a) Sky-aware scenes (clock+sky bg, moon_phase, iss_pass,
//       jupiter_visibility) can read the snapshot instead of running
//       the full trig stack on every frame — moves float-heavy work
//       off the render hot path (NFR-1.3 spirit) without changing
//       the scene contract.
//   (b) The chrome layer can carry ambient micro-indicators (1-px
//       sun-arc dot along the top edge) without burdening Core 0.
//
// Cross-core path uses the FR-16.7 SeqSnapshot — single-producer
// (Core 1 in tick()), multi-consumer (any reader). NO mutex; the
// snapshot is read every frame on the render hot path and must not
// stall on Core 0's network jitter.
//
// Lifecycle:
//   setup() Core 0 → sky_snapshot::init()        (seeds an empty snap)
//   loop1() Core 1 → sky_snapshot::tick(now_ms, slack_ms)
//                       — internally rate-limited to ~1 Hz
//                       — skips if slack_ms < kSlackFloorMs (FR-16.9)
//   render path    → sky_snapshot::read(&snap)   (lock-free)
//
// (added in phase D.6 — FR-16.5 background sky simulation)

#pragma once

#include <stdint.h>

namespace sky_snapshot {

struct Snapshot {
  // Monotonic — bumped on every successful refresh. Readers that want
  // "exactly once per change" semantics keep their own last_seen.
  uint32_t version;

  // false until the first refresh against a valid RTC has succeeded.
  // Pre-RTC-sync (FR-9.6) the sky model has no time anchor, so all
  // fields below are meaningless.
  bool     valid;

  // ── Sun (always populated when valid) ───────────────────────────
  float    sun_altitude_deg;   // -90..+90 (negative = below horizon)
  float    sun_azimuth_deg;    // 0..360 (0=N, 90=E, 180=S, 270=W)
  bool     observer_dark;      // sun.altitude <= -6° (twilight or darker)

  // ── Moon (always populated when valid) ──────────────────────────
  // Closed-form synodic-month math from the RTC; same calculation
  // moon_phase_scene falls back to when no fresh observatory/moon
  // push exists. Moon altitude/azimuth requires HA-supplied RA/Dec
  // and is OUT OF SCOPE for D.6 (see FR-14.4).
  float    moon_phase_frac;    // 0..1 (0=new, 0.5=full, 0.99=waning crescent)
  uint8_t  moon_illum_pct;     // 0..100
  uint8_t  moon_age_d;         // 0..29 days since last new moon

  // ── ISS (populated when iss_state has a fresh push) ─────────────
  bool     iss_have_position;  // false → no fresh iss_state snapshot
  float    iss_bearing_deg;    // 0..360, same convention as sun_azimuth
  float    iss_elevation_deg;  // -90..+90
  bool     iss_visible;        // sunlit && observer_dark && elev>=0

  // millis() at the moment of the last refresh. Lets readers detect
  // a stuck snapshot (hasn't ticked recently → Core 1 is busy or
  // sky-model is gated off by low slack).
  uint32_t computed_at_ms;
};

// Seed an initial empty snapshot so first-frame readers don't observe
// uninitialised seqlock state. Call from setup() before either core
// spins.
void init();

// Producer (Core 1 ONLY — invokes sun::compute + iss_geom::look_angles
// which use float math; per CODING_PRACTICES §3 these must NOT run on
// Core 0). Internally rate-limited to ~1 Hz; gated on slack_ms >=
// RENDER_SLACK_FLOOR_MS_DEFAULT to preserve FR-3.1's frame-rate
// target under load (FR-16.9). Returns true if a refresh actually
// ran this call.
bool tick(uint32_t now_ms, uint32_t slack_ms);

// Reader (any core). Lock-free seqlock read. Always copies the full
// snapshot byte-for-byte. Returns the version number observed; the
// caller's `out->valid` indicates whether the values are meaningful.
uint32_t read(Snapshot* out);

}  // namespace sky_snapshot

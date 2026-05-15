// Sun position (altitude + azimuth) from epoch + observer lat/lon.
//
// Uses the NOAA low-precision algorithm (Spencer 1971 / Michalsky
// 1988 family). Sub-1° accurate, plenty for a 64×32 panel — the
// whole sky maps to ~64 px so 1° error = ~0.2 px.
//
// Output convention:
//   altitude_deg : -90 (nadir) .. +90 (zenith). 0 = horizon.
//   azimuth_deg  : 0 = North, 90 = East, 180 = South, 270 = West.
//
// Cost: ~10 sin/cos/asin per call. Cheap on RP2040 with hardware
// FPU absent (libm soft-float ~30 µs each), so call at most once per
// frame — typically caching once a minute is plenty.
//
// (added in phase 6.5+ polish)

#pragma once

#include <stdint.h>

namespace sun {

struct Position {
  float altitude_deg;  // -90..+90 (negative = below horizon)
  float azimuth_deg;   // 0..360 (0=N, 90=E, 180=S, 270=W)
};

// Compute sun position for a given UTC Unix epoch + observer.
// Pure function — no global state.
Position compute(int32_t utc_epoch, float latitude_deg, float longitude_deg);

}  // namespace sun

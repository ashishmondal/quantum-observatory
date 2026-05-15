// LUT storage + setup-time precomputation. Float math is restricted to this
// init function; runtime trig is integer LUT lookup only. (NFR-1.3)
//
// (added in phase 2.1)

#include "fixed_point.h"

#include <math.h>

namespace fp {

int16_t g_sin_lut[LUT_SIZE];

void sin_cos_lut_init() {
  // 2π / 256 in float — used once at boot, never per-frame.
  constexpr float TWO_PI_F = 6.2831853071795864769f;
  for (int i = 0; i < LUT_SIZE; ++i) {
    const float theta = (TWO_PI_F * i) / LUT_SIZE;
    g_sin_lut[i] = static_cast<int16_t>(sinf(theta) * Q8_ONE);
  }
}

}  // namespace fp

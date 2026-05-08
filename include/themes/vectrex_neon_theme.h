// VECTREX_NEON — Atari/Vectrex vector arcade (THEME.md §2.3).
//
// Animated clock background: dual perspective grid (floor + ceiling)
// with a fixed 7-ray fan and 5 forward-sliding scan-lines per half.
// Carries a magenta/cyan duotone bg ramp.

#pragma once

#include "theme.h"

namespace theme {

class VectrexNeonTheme : public Theme {
 public:
  VectrexNeonTheme();
  void   render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) override;
  Melody melody() const override;
};

extern VectrexNeonTheme g_vectrex_neon_theme;

}  // namespace theme

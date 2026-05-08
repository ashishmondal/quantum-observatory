// NOSTROMO_GREEN — 80s Alien CRT (THEME.md §2.2).
//
// Animated clock background: every-other-column "memory dump" drops
// with a 4-row gradient tail and per-column step staggering. Carries
// a green duotone bg ramp.

#pragma once

#include <stdint.h>

#include "config.h"
#include "theme.h"

namespace theme {

class NostromoGreenTheme : public Theme {
 public:
  NostromoGreenTheme();
  void   init_clock_bg() override;
  void   render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) override;
  Melody melody() const override;

 private:
  uint32_t next_rand();
  uint32_t m_rng = 0xA5F03C2Du;
  uint16_t m_col_step_ms[PANEL_WIDTH] = {};
  uint8_t  m_col_phase[PANEL_WIDTH]   = {};
};

extern NostromoGreenTheme g_nostromo_green_theme;

}  // namespace theme

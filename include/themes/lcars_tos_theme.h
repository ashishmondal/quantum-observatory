// LCARS_TOS — Star Trek LCARS-precursor (THEME.md §2.5).
//
// Animated clock background: 8×4 grid of 6×6 cells; one random cell
// toggles every 500 ms between an LCARS palette color (5-stop orange
// → brown ramp matching the reference swatch) and black. Each cell
// remembers its own shade so the panel reads as a multi-tier bridge
// sub-processor display instead of a uniform dim wash. Carries an
// orange/yellow duotone bg ramp.

#pragma once

#include <stdint.h>

#include "theme.h"

namespace theme {

class LcarsTosTheme : public Theme {
 public:
  LcarsTosTheme();
  void   init_clock_bg() override;
  void   render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) override;
  Melody melody() const override;

 private:
  uint32_t next_rand();
  uint32_t m_rng         = 0xA5F03C2Du;
  uint32_t m_cells       = 0;   // bit i = cell (i % 8, i / 8)
  uint32_t m_last_toggle = 0;
  uint8_t  m_color_idx[32] = {0};  // palette index per cell
};

extern LcarsTosTheme g_lcars_tos_theme;

}  // namespace theme

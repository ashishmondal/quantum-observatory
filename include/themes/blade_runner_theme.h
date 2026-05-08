// BLADE_RUNNER — late-80s neo-noir HUD (THEME.md §2.4).
//
// Animated clock background: diagonal cyan/magenta rain (1:4) with a
// 3-pixel trail per drop and a 1-frame +2 px global x-shift every
// ~100 frames. Carries an orange/cyan duotone bg ramp.

#pragma once

#include <stdint.h>

#include "theme.h"

namespace theme {

class BladeRunnerTheme : public Theme {
 public:
  BladeRunnerTheme();
  void   init_clock_bg() override;
  void   render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) override;
  Melody melody() const override;

 private:
  uint32_t next_rand();
  void     respawn(int i, bool initial);

  static constexpr int kRain = 18;
  uint32_t m_rng = 0xA5F03C2Du;
  int8_t   m_x[kRain]      = {};
  int8_t   m_y[kRain]      = {};
  uint8_t  m_color[kRain]  = {};
  uint32_t m_frame_count   = 0;
};

extern BladeRunnerTheme g_blade_runner_theme;

}  // namespace theme

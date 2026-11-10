// HAIL_MARY — Project Hail Mary (THEME.md §2.7).
//
// Petrova-line crimson on deep space black with xenonite silver-blue
// body text. Animated clock background: the infrared Petrova line
// arcing out of a dim star with Astrophage specks riding it.

#pragma once

#include "theme.h"

namespace theme {

class HailMaryTheme : public Theme {
 public:
  HailMaryTheme();
  void   render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) override;
  Melody melody() const override;
};

extern HailMaryTheme g_hail_mary_theme;

}  // namespace theme

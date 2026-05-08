// APOLLO_AMBER — 70s NASA MOCR (THEME.md §2.1). Default theme.
//
// Carries no bg_ramp (image backgrounds stay in passthrough mode —
// FR-15.6). Animated clock background: a single amber dot scanning
// the panel like a CRT electron beam, with a 3-scanline gradient
// trail through the STAR_AMBER palette ramp.

#pragma once

#include "theme.h"

namespace theme {

class ApolloAmberTheme : public Theme {
 public:
  ApolloAmberTheme();
  void   render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) override;
  Melody melody() const override;
};

extern ApolloAmberTheme g_apollo_amber_theme;

}  // namespace theme

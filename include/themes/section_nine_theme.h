// SECTION_NINE — Ghost in the Shell 2017 (THEME.md §2.6).
//
// Hot magenta/pink on deep teal with white pop and a cyan secondary
// accent. Defining motif: Major's cyberspace dive sequence — vertical
// data shafts climb UP the panel at varied speeds. Each shaft is led
// by a hand-pixel 5×7 katakana glyph (the Matrix-code signal that
// also names the theme as Japanese), trailing a 2-px-wide mid-then-
// dim column behind. 4:1 magenta:cyan colour mix gives the field hue
// variation without abandoning the theme identity. Deterministic LCG
// drives x-position, speed, colour, and glyph index so the field
// looks chaotic but is reproducible.
//
// Distinct from BLADE_RUNNER's diagonal 1-px rain: motion is strictly
// vertical-UP (rain is +/+ diagonal-down), shafts are led by 5×7
// glyphs (rain heads are 1-px dots), and the trails are solid 2-px
// columns (rain trails are 1-px points).
//
// An occasional 1-frame +/- 2 px horizontal tear of the whole field
// fires every ~120 frames — hologram signal loss.

#pragma once

#include <stdint.h>

#include "theme.h"

namespace theme {

class SectionNineTheme : public Theme {
 public:
  SectionNineTheme();
  void   init_clock_bg() override;
  void   render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) override;
  Melody melody() const override;

 private:
  uint32_t next_rand();
  void     respawn(int i, bool initial);

  static constexpr int kShafts = 12;
  uint32_t m_rng         = 0xD3E72A91u;
  uint32_t m_frame_count = 0;
  int8_t   m_x[kShafts]     = {};   // glyph-left x (5-px-wide glyph fits)
  int8_t   m_y[kShafts]     = {};   // y of the glyph's TOP row (the head)
  uint8_t  m_speed[kShafts] = {};   // 1 or 2 px per frame
  uint8_t  m_color[kShafts] = {};   // 0 = magenta, 1 = cyan
  uint8_t  m_glyph[kShafts] = {};   // index into kGlyphs[]
};

extern SectionNineTheme g_section_nine_theme;

}  // namespace theme

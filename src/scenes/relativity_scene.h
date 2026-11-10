// relativity scene (FR-21.4) — real-world time-dilation examples.
//
// Offline facts table. The divider row under the header carries two
// "clocks" racing side by side — the right one ticks slower — as a
// visual cue for "time runs at different speeds".
//
// Numbers (order-of-magnitude, rounded for a 16-glyph line):
//   GPS: +45 µs/day (gravity) − 7 µs/day (speed) = +38 µs/day.
//   ISS: ~0.007 s younger per 6 months aboard (speed wins over altitude).
//   Everest (8.8 km): g·h/c² ≈ 1.1e-13/km → ~+80 ns/day.
//   Muons at 0.998c: γ ≈ 16.
//   1.4 M☉, 12 km neutron star: √(1 − r_s/r) ≈ 0.81 → ~20 % slower.

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "scene.h"
#include "theme.h"

namespace relativity_detail {

struct Fact { const char* a; const char* b; const char* c; };

inline constexpr Fact kFacts[] = {
  { "GPS SATELLITES", "CLOCKS RUN FAST", "+38 US EVERY DAY" },
  { "WITHOUT FIX",    "GPS DRIFTS",      "~10 KM A DAY" },
  { "ISS ASTRONAUTS", "AGE SLOWER",      "-7 MS / 6 MONTHS" },
  { "MT EVEREST",     "TIME RUNS FASTER", "+80 NS PER DAY" },
  { "COSMIC MUONS",   "AT 99.8% OF C",   "LIVE 16X LONGER" },
  { "NEUTRON STAR",   "SURFACE CLOCKS",  "~20% SLOWER" },
  { "BLACK HOLE",     "AT THE HORIZON",  "TIME FREEZES*" },
  { "*SEEN FROM",     "FAR AWAY",        "EINSTEIN 1915" },
};
inline constexpr uint8_t kCount = sizeof(kFacts) / sizeof(kFacts[0]);

}  // namespace relativity_detail

class RelativityScene : public Scene {
 public:
  const char* name() const override { return "relativity"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    using namespace relativity_detail;
    matrix.fillScreen(0x0000);
    draw_twin_clocks(matrix, now_ms);
    cards::header(matrix, "TIME", now_ms);

    const Fact& f = kFacts[m_deck.index()];
    cards::Card c;
    cards::set(c, 0, theme::ink(theme::Ink::VALUE), "%s", f.a);
    cards::set(c, 1, theme::ink(theme::Ink::STATUS_INFO), "%s", f.b);
    cards::set(c, 2, theme::ink(theme::Ink::ACCENT_MAGENTA), "%s", f.c);
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), kCount);
    cards::draw(matrix, c, e);
  }

 private:
  // Two 30-px sweeps on row 8: left at 1 rev / 3 s, right at 1 rev / 4 s.
  static void draw_twin_clocks(Adafruit_Protomatter& m, uint32_t now_ms) {
    const uint16_t dim  = theme::ink(theme::Ink::DIVIDER);
    const uint16_t fast = theme::ink(theme::Ink::STATUS_OK);
    const uint16_t slow = theme::ink(theme::Ink::STATUS_WARN);
    m.drawFastHLine(0, 8, 30, dim);
    m.drawFastHLine(34, 8, 30, dim);
    const int16_t a = static_cast<int16_t>((now_ms % 3000u) * 30u / 3000u);
    const int16_t b = static_cast<int16_t>((now_ms % 4000u) * 30u / 4000u);
    m.drawFastHLine(0, 8, a, fast);
    m.drawFastHLine(34, 8, b, slow);
  }

  cards::Deck m_deck;
};

// light_travel scene (FR-21.4) — "how long ago did this light leave?"
//
// Fully offline: a flash-resident table of objects from the Moon to
// the cosmic microwave background. Each card shows the object, its
// light-travel time, and — for anything more than a year away — the
// calendar year the light we see tonight set off (computed from the
// RTC year, so it stays true as the years roll on). Two Project Hail
// Mary easter eggs ride along (Tau Ceti, 40 Eridani).

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "scene.h"
#include "theme.h"
#include "time_of_day.h"

namespace light_travel_detail {

struct Obj {
  const char* name;
  const char* travel;     // light-travel time, display text
  int32_t     years;      // whole years ago (0 ⇒ use `fact`)
  const char* fact;
};

inline constexpr Obj kObjs[] = {
  { "THE MOON",    "1.3 SECONDS",  0,          "YOU SEE THE PAST" },
  { "THE SUN",     "8 MIN 20 SEC", 0,          "OLD SUNSHINE" },
  { "JUPITER",     "~43 MINUTES",  0,          "VARIES W/ ORBIT" },
  { "VOYAGER 1",   "~1 LIGHT-DAY", 0,          "FARTHEST PROBE" },
  { "PROXIMA CEN", "4.2 YEARS",    4,          nullptr },
  { "TAU CETI",    "11.9 YEARS",   12,         nullptr },
  { "40 ERIDANI",  "16.3 YEARS",   16,         nullptr },
  { "VEGA",        "25 YEARS",     25,         nullptr },
  { "BETELGEUSE",  "~550 YEARS",   550,        nullptr },
  { "ORION NEB",   "1,344 YEARS",  1344,       nullptr },
  { "GALAXY CORE", "26,000 YRS",   26000,      nullptr },
  { "ANDROMEDA",   "2.5M YEARS",   0,          "BEFORE HUMANS" },
  { "M87 BLK HOLE","55M YEARS",    0,          "AFTER DINOSAURS" },
  { "CMB GLOW",    "13.8B YEARS",  0,          "OLDEST LIGHT" },
};
inline constexpr uint8_t kCount = sizeof(kObjs) / sizeof(kObjs[0]);

}  // namespace light_travel_detail

class LightTravelScene : public Scene {
 public:
  const char* name() const override { return "light_travel"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    using namespace light_travel_detail;
    matrix.fillScreen(0x0000);
    draw_photon(matrix, now_ms);
    cards::header(matrix, "LITE", now_ms);

    cards::Card c;
    fill(m_deck.index(), now_ms, c);
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), kCount);
    cards::draw(matrix, c, e);
  }

 private:
  void fill(uint8_t i, uint32_t now_ms, cards::Card& c) const {
    using namespace light_travel_detail;
    const Obj& o = kObjs[i];
    cards::set(c, 0, theme::ink(theme::Ink::VALUE), "%s", o.name);
    cards::set(c, 1, theme::ink(theme::Ink::STATUS_INFO), "%s", o.travel);
    const tod::Reading r = tod::now(now_ms);
    if (o.years > 0 && r.valid) {
      int16_t yr; uint8_t mo, d, dow;
      tod::date_from_local_epoch(r.local_epoch, &yr, &mo, &d, &dow);
      const int32_t left = static_cast<int32_t>(yr) - o.years;
      if (left > 0) {
        cards::set(c, 2, theme::ink(theme::Ink::STATUS_WARN), "LEFT IN %ld",
                   static_cast<long>(left));
      } else {
        cards::set(c, 2, theme::ink(theme::Ink::STATUS_WARN), "LEFT %ldBC",
                   static_cast<long>(1 - left));
      }
    } else {
      cards::set(c, 2, theme::ink(theme::Ink::STATUS_WARN), "%s",
                 o.fact != nullptr ? o.fact : "");
    }
  }

  // A single photon streaking along the bottom edge — subtle motion so
  // the panel never looks frozen between cards.
  static void draw_photon(Adafruit_Protomatter& m, uint32_t now_ms) {
    const int16_t x = static_cast<int16_t>((now_ms / 40u) % (PANEL_WIDTH + 12)) - 6;
    const uint16_t head = theme::ink(theme::Ink::ACCENT);
    const uint16_t tail = theme::ink(theme::Ink::DIVIDER);
    for (int16_t k = 0; k < 6; ++k) {
      const int16_t px = x - k;
      if (px >= 0 && px < PANEL_WIDTH) m.drawPixel(px, 8, k == 0 ? head : tail);
    }
  }

  cards::Deck m_deck;
};

// sky_now scene (FR-21.2) — "what bright things are up right now?"
//
// HA's skyfield publisher pushes observatory/sky every 10 min: up to
// five naked-eye objects (Moon, planets, the brightest stars) above
// 10°, brightest first, with azimuth + altitude. The firmware turns
// azimuth into an 8-point compass label and shows three per card.
// Visual: a mini horizon dome at the right with one dot per object,
// placed by azimuth (x) and altitude (y).

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "feeds.h"
#include "scene.h"
#include "theme.h"

class SkyNowScene : public Scene {
 public:
  const char* name() const override { return "sky_now"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    matrix.fillScreen(0x0000);
    cards::header(matrix, "SKY", now_ms);

    feeds::Sky s{};
    const bool fresh = feeds::get(now_ms, &s);
    cards::Card c;
    cards::clear(c);
    uint8_t count = 1;
    if (!fresh) {
      cards::set(c, 0, theme::ink(theme::Ink::STATUS_STALE), "WAIT");
      cards::set(c, 1, theme::ink(theme::Ink::STATUS_INFO), "NO SKY DATA");
    } else if (s.n == 0) {
      cards::set(c, 0, theme::ink(theme::Ink::VALUE), "NOTHING BRIGHT");
      cards::set(c, 1, theme::ink(theme::Ink::STATUS_INFO), "IS UP RIGHT NOW");
      cards::set(c, 2, theme::ink(theme::Ink::STATUS_DIM), "TRY AFTER DARK");
    } else {
      count = static_cast<uint8_t>((s.n + 2) / 3);
      const uint8_t base = static_cast<uint8_t>((m_deck.index() % count) * 3);
      for (uint8_t k = 0; k < 3 && base + k < s.n; ++k) {
        const feeds::SkyObj& o = s.obj[base + k];
        cards::set(c, k, k == 0 ? theme::ink(theme::Ink::VALUE)
                                : theme::ink(theme::Ink::STATUS_INFO),
                   "%-7s %-2s%2d", o.name, cards::compass8(o.az_deg), o.alt_deg);
      }
      draw_dome(matrix, s, now_ms);
    }
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), count);
    cards::draw(matrix, c, e, 1, /*backdrop=*/true);
  }

 private:
  // 15-px-wide dome in x=49..63, horizon on row 31, zenith at row 17.
  static void draw_dome(Adafruit_Protomatter& m, const feeds::Sky& s, uint32_t now_ms) {
    m.drawFastHLine(49, 31, 15, theme::ink(theme::Ink::DIVIDER));
    for (uint8_t k = 0; k < s.n; ++k) {
      const int16_t x = static_cast<int16_t>(49 + (s.obj[k].az_deg * 14) / 359);
      const int16_t y = static_cast<int16_t>(30 - (s.obj[k].alt_deg * 13) / 90);
      const bool blink = k == 0 && ((now_ms / 500u) & 1u);
      m.drawPixel(x, y, blink ? theme::ink(theme::Ink::ACCENT)
                              : theme::ink(theme::Ink::STATUS_WARN));
    }
  }

  cards::Deck m_deck;
};

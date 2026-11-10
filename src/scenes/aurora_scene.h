// aurora scene (FR-21.5) — NOAA planetary Kp + animated aurora curtain.
//
// HA relays NOAA SWPC's planetary K-index (now + 24 h forecast max)
// on observatory/aurora. The firmware decides locally what Kp this
// latitude needs (config.h LATITUDE_DEG) and gives a verdict. The
// curtain brightness scales with Kp so a storm is visibly different
// from a quiet night even before the text types out.

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "config.h"
#include "feeds.h"
#include "fixed_point.h"
#include "scene.h"
#include "theme.h"

class AuroraScene : public Scene {
 public:
  const char* name() const override { return "aurora"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  // Rough geographic-latitude → Kp needed for a naked-eye aurora on
  // the poleward horizon (NOAA "aurora viewline" rule of thumb).
  static uint8_t kp_needed() {
    const float lat = LATITUDE_DEG < 0 ? -LATITUDE_DEG : LATITUDE_DEG;
    if (lat >= 60) return 2;
    if (lat >= 55) return 3;
    if (lat >= 50) return 4;
    if (lat >= 45) return 5;
    if (lat >= 40) return 6;
    if (lat >= 35) return 7;
    if (lat >= 30) return 8;
    return 9;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    feeds::Aurora a{};
    const bool fresh = feeds::get(now_ms, &a);
    matrix.fillScreen(0x0000);
    draw_curtain(matrix, now_ms, fresh ? a.kp_now_x10 : 20);
    cards::header(matrix, "AUR", now_ms, theme::Ink::STATUS_OK, theme::Ink::STATUS_OK_DIM);

    const uint8_t count = fresh ? 4 : 2;
    cards::Card c;
    fill(fresh, a, m_deck.index() % count, c);
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), count);
    cards::draw(matrix, c, e, 2, /*backdrop=*/true);
  }

 private:
  static void fill(bool fresh, const feeds::Aurora& a, uint8_t i, cards::Card& c) {
    const uint16_t v  = theme::ink(theme::Ink::VALUE);
    const uint16_t ok = theme::ink(theme::Ink::STATUS_OK);
    const uint16_t wn = theme::ink(theme::Ink::STATUS_WARN);
    const uint16_t in = theme::ink(theme::Ink::STATUS_INFO);
    const uint8_t k = fresh ? i : static_cast<uint8_t>(i + 2);
    const uint8_t need = kp_needed();
    switch (k) {
      case 0: {
        cards::set(c, 0, v, "KP %u.%u NOW", a.kp_now_x10 / 10u, a.kp_now_x10 % 10u);
        cards::set(c, 1, in, "24H MAX %u.%u", a.kp_max24_x10 / 10u, a.kp_max24_x10 % 10u);
        const uint8_t kp = static_cast<uint8_t>(a.kp_now_x10 / 10u);
        if (kp >= 5)      cards::set(c, 2, wn, "G%u STORM!", static_cast<unsigned>(kp - 4));
        else if (kp >= 4) cards::set(c, 2, ok, "ACTIVE");
        else              cards::set(c, 2, ok, "QUIET SUN");
        break;
      }
      case 1: {
        cards::set(c, 0, v, "HERE YOU NEED");
        cards::set(c, 1, in, "KP %u OR MORE", static_cast<unsigned>(need));
        if (a.kp_max24_x10 >= need * 10u) cards::set(c, 2, wn, "WATCH TONIGHT!");
        else                               cards::set(c, 2, ok, "NOT TONIGHT");
        break;
      }
      case 2:
        cards::set(c, 0, v, "AURORA =");
        cards::set(c, 1, in, "SOLAR WIND");
        cards::set(c, 2, ok, "HITTING OUR AIR");
        break;
      default:
        cards::set(c, 0, v, "MAY 10 2024");
        cards::set(c, 1, in, "KP 9 STORM");
        cards::set(c, 2, wn, "SEEN IN TEXAS!");
        break;
    }
  }

  // Swaying curtain hanging from row 9. Height and brightness follow
  // two slow sine waves; overall gain follows Kp (×10, 0..90).
  static void draw_curtain(Adafruit_Protomatter& m, uint32_t now_ms, uint8_t kp_x10) {
    const int32_t gain = 60 + kp_x10 * 2;   // 60..240 of 255
    const uint8_t t1 = static_cast<uint8_t>(now_ms / 40u);
    const uint8_t t2 = static_cast<uint8_t>(now_ms / 63u);
    for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
      const int16_t s1 = fp::sin_q8(static_cast<uint8_t>(x * 6 + t1));
      const int16_t s2 = fp::sin_q8(static_cast<uint8_t>(x * 11 - t2));
      const int16_t h  = static_cast<int16_t>(9 + ((s1 + s2 + 512) * 12) / 1024);
      const int32_t col_v = (gain * (300 + s2)) / 556;
      for (int16_t y = 9; y < 9 + h && y < PANEL_HEIGHT; ++y) {
        const int32_t fade = (col_v * (h - (y - 9))) / h;
        const uint8_t g = static_cast<uint8_t>(fade > 255 ? 255 : fade);
        // Violet fringe on the top two rows, green body below.
        uint16_t px;
        if (y - 9 < 2) {
          px = static_cast<uint16_t>(((g >> 4) << 11) | ((g >> 5) << 5) | (g >> 3));
        } else {
          px = static_cast<uint16_t>(((g >> 6) << 11) | ((g >> 2) << 5) | (g >> 5));
        }
        m.drawPixel(x, y, px);
      }
    }
  }

  cards::Deck m_deck;
};

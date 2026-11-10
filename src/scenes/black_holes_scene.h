// black_holes scene (FR-21.6) — latest gravitational-wave detection.
//
// HA polls GraceDB for the newest *significant* public superevent and
// pushes observatory/gw (id, source type, mean distance, age, count).
// Cards walk through what merged, how far away, when the wave set off
// (distance in light-years == years ago), then a few fixed LIGO facts.
// When the feed is stale the scene still runs on the offline facts.
//
// Visual: an inspiral in the right-hand column — two dots orbit ever
// faster and closer, merge in a flash, then rings ripple outward.

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "feeds.h"
#include "fixed_point.h"
#include "scene.h"
#include "theme.h"

class BlackHolesScene : public Scene {
 public:
  const char* name() const override { return "black_holes"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    matrix.fillScreen(0x0000);
    draw_inspiral(matrix, now_ms);
    cards::header(matrix, "GW", now_ms);

    feeds::Gw gw{};
    const bool fresh = feeds::get(now_ms, &gw);
    const uint8_t count = fresh ? 6 : 3;
    cards::Card c;
    fill(fresh, gw, m_deck.index() % count, c);
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), count);
    cards::draw(matrix, c, e, 2, /*backdrop=*/true);
  }

 private:
  static void fmt_dist(char* out, size_t cap, uint16_t mly) {
    if (mly >= 1000) {
      snprintf(out, cap, "%u.%uB", static_cast<unsigned>(mly / 1000),
               static_cast<unsigned>((mly % 1000) / 100));
    } else {
      snprintf(out, cap, "%uM", static_cast<unsigned>(mly));
    }
  }

  static void fill(bool fresh, const feeds::Gw& gw, uint8_t i, cards::Card& c) {
    const uint16_t v  = theme::ink(theme::Ink::VALUE);
    const uint16_t in = theme::ink(theme::Ink::STATUS_INFO);
    const uint16_t ac = theme::ink(theme::Ink::ACCENT_MAGENTA);
    // Offline cards come last when fresh, first (only) when stale.
    const uint8_t k = fresh ? i : static_cast<uint8_t>(i + 3);
    char d[10];
    switch (k) {
      case 0:
        cards::set(c, 0, v, "LATEST HIT");
        cards::set(c, 1, in, "%s", gw.id);
        cards::set(c, 2, ac, "%ld DAYS AGO", static_cast<long>(gw.days_ago));
        break;
      case 1:
        switch (gw.type) {
          case feeds::GwType::BBH:
            cards::set(c, 0, v, "2 BLACK HOLES"); break;
          case feeds::GwType::BNS:
            cards::set(c, 0, v, "2 NEUTRON STARS"); break;
          case feeds::GwType::NSBH:
            cards::set(c, 0, v, "BH + NEUTRON"); break;
          default:
            cards::set(c, 0, v, "MYSTERY PAIR"); break;
        }
        cards::set(c, 1, in, "CRASHED INTO");
        cards::set(c, 2, ac, "EACH OTHER");
        break;
      case 2:
        fmt_dist(d, sizeof(d), gw.dist_mly);
        cards::set(c, 0, v, "WAVE TRAVELED");
        cards::set(c, 1, in, "%s LIGHT YRS", d);
        if (gw.dist_mly >= 4600) {
          cards::set(c, 2, ac, "OLDER THAN EARTH");
        } else {
          cards::set(c, 2, ac, "%u ALERTS SO FAR", static_cast<unsigned>(gw.count));
        }
        break;
      case 3:
        cards::set(c, 0, v, "FIRST EVER");
        cards::set(c, 1, in, "GW150914");
        cards::set(c, 2, ac, "SEPT 14 2015");
        break;
      case 4:
        cards::set(c, 0, v, "LIGO ARMS");
        cards::set(c, 1, in, "4 KM LONG");
        cards::set(c, 2, ac, "STRETCH 1E-18 M");
        break;
      default:
        cards::set(c, 0, v, "SPACE ITSELF");
        cards::set(c, 1, in, "RINGS LIKE");
        cards::set(c, 2, ac, "A STRUCK BELL");
        break;
    }
  }

  // 6 s cycle in a 17×22 box at the right edge: 4 s inspiral, flash,
  // then 2 expanding rings.
  static void draw_inspiral(Adafruit_Protomatter& m, uint32_t now_ms) {
    constexpr int16_t cx = 55, cy = 20;
    const uint32_t t = now_ms % 6000u;
    const uint16_t a_ink = theme::ink(theme::Ink::STATUS_INFO);
    const uint16_t b_ink = theme::ink(theme::Ink::ACCENT_MAGENTA);
    if (t < 4000u) {
      // radius 7 → 1 px (Q8), angular speed grows as the pair tightens.
      const int32_t r_q8  = 7 * 256 - static_cast<int32_t>(t) * 6 * 256 / 4000;
      const uint8_t angle = static_cast<uint8_t>((t * t) / 9000u);
      const int16_t dx = static_cast<int16_t>((r_q8 * fp::cos_q8(angle)) >> 16);
      const int16_t dy = static_cast<int16_t>((r_q8 * fp::sin_q8(angle)) >> 16);
      m.drawPixel(cx + dx, cy + dy, a_ink);
      m.drawPixel(cx - dx, cy - dy, b_ink);
    } else if (t < 4200u) {
      m.fillCircle(cx, cy, 2, theme::ink(theme::Ink::ACCENT));
    } else {
      const int16_t r = static_cast<int16_t>((t - 4200u) * 10u / 1800u) + 1;
      m.drawCircle(cx, cy, r, theme::ink(theme::Ink::DIVIDER));
      if (r > 4) m.drawCircle(cx, cy, r - 4, theme::ink(theme::Ink::LABEL));
      m.drawPixel(cx, cy, theme::ink(theme::Ink::ACCENT));
    }
  }

  cards::Deck m_deck;
};

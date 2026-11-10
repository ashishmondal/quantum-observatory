// weather scene (FR-21.1) — the morning "what to wear" glance and the
// evening "can I stargaze tonight?" verdict.
//
// HA pushes observatory/weather (current temp, today's hi/lo, rain
// chance, tonight's cloud cover, short condition). The firmware picks
// the cards by local hour:
//   before noon → NOW / HI-LO / RAIN, then WEAR advice
//   after noon  → TONIGHT clouds + stargazing verdict, then NOW temp
// Clothing advice and the stargazing verdict are derived on-device
// from the numbers, so HA stays a pass-through. A small condition
// icon sits in the right-hand column.

#pragma once

#include <string.h>

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "feeds.h"
#include "scene.h"
#include "theme.h"
#include "time_of_day.h"

class WeatherScene : public Scene {
 public:
  const char* name() const override { return "weather"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    matrix.fillScreen(0x0000);
    feeds::Weather w{};
    const bool fresh = feeds::get(now_ms, &w);
    const tod::Reading r = tod::now(now_ms);
    const bool morning = r.valid && r.hour < 12;
    const bool night   = r.valid && (r.hour >= 18 || r.hour < 7);

    draw_icon(matrix, now_ms, fresh ? w.cond : "", night);
    cards::header(matrix, "WX", now_ms);

    cards::Card c;
    cards::clear(c);
    const uint16_t v  = theme::ink(theme::Ink::VALUE);
    const uint16_t in = theme::ink(theme::Ink::STATUS_INFO);
    const uint16_t ok = theme::ink(theme::Ink::STATUS_OK);
    const uint16_t wn = theme::ink(theme::Ink::STATUS_WARN);
    if (!fresh) {
      cards::set(c, 0, theme::ink(theme::Ink::STATUS_STALE), "WAIT");
      cards::set(c, 1, in, "NO WEATHER");
      cards::set(c, 2, in, "FROM HA YET");
      const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), 1);
      cards::draw(matrix, c, e);
      return;
    }

    const uint8_t i = m_deck.index() % 2;
    if ((i == 0) == morning) {
      cards::set(c, 0, v, "%dF %s", w.temp_f, w.cond);
      cards::set(c, 1, in, "HI %d LO %d", w.hi_f, w.lo_f);
      cards::set(c, 2, w.precip_pct >= 40 ? wn : ok, "RAIN %u%%", w.precip_pct);
      if (!morning) {
        cards::set(c, 0, v, "NOW %dF", w.temp_f);
        cards::set(c, 1, in, "%s", w.cond);
        cards::set(c, 2, in, "LOW %dF", w.lo_f);
      }
    } else if (morning) {
      const int16_t cold = w.temp_f < w.lo_f ? w.temp_f : w.lo_f;
      cards::set(c, 0, v, "WEAR:");
      cards::set(c, 1, wn, "%s", wear_for(cold, w.hi_f));
      cards::set(c, 2, w.precip_pct >= 40 ? wn : ok, "%s",
                 w.precip_pct >= 40 ? "+ UMBRELLA" : "NO UMBRELLA");
    } else {
      cards::set(c, 0, v, "TONIGHT SKY");
      cards::set(c, 1, in, "CLOUDS %u%%", w.cloud_tonight_pct);
      if (w.cloud_tonight_pct < 30)      cards::set(c, 2, ok, "STARGAZE: YES");
      else if (w.cloud_tonight_pct < 65) cards::set(c, 2, wn, "STARGAZE: MAYBE");
      else                               cards::set(c, 2, theme::ink(theme::Ink::STATUS_DIM), "STARGAZE: NO");
    }
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), 2);
    cards::draw(matrix, c, e, 2, /*backdrop=*/true);
  }

 private:
  static const char* wear_for(int16_t cold_f, int16_t hi_f) {
    if (cold_f <= 40) return "HEAVY COAT";
    if (cold_f <= 55) return hi_f >= 75 ? "LAYERS" : "JACKET";
    if (cold_f <= 67) return "HOODIE";
    return "T-SHIRT";
  }

  // 13×13 icon centred at (56, 20).
  static void draw_icon(Adafruit_Protomatter& m, uint32_t now_ms, const char* cond,
                        bool night) {
    constexpr int16_t cx = 56, cy = 20;
    const bool rain   = strstr(cond, "RAIN") || strstr(cond, "STORM") || strstr(cond, "SHOWER");
    const bool cloudy = rain || strstr(cond, "CLOUD") || strstr(cond, "FOG") || strstr(cond, "CLDY");
    const bool partly = strstr(cond, "PART") != nullptr;
    const uint16_t sun   = 0xFE60;
    const uint16_t moon  = 0xCE79;
    const uint16_t cloud = 0x8C71;
    if (!cloudy || partly) {
      if (night) {
        m.fillCircle(cx - 2, cy - 3, 4, moon);
        m.fillCircle(cx, cy - 4, 3, 0x0000);
        if (((now_ms / 700u) & 1u) == 0u) m.drawPixel(cx + 5, cy - 6, moon);
        m.drawPixel(cx + 3, cy + 2, moon);
      } else {
        m.fillCircle(cx - 2, cy - 3, 3, sun);
        const uint8_t rot = static_cast<uint8_t>((now_ms / 400u) & 1u);
        for (int8_t k = 0; k < 4; ++k) {
          const int8_t dx = (k == 0) ? 5 : (k == 1) ? -5 : 0;
          const int8_t dy = (k == 2) ? 5 : (k == 3) ? -5 : 0;
          m.drawPixel(cx - 2 + dx + rot, cy - 3 + dy, sun);
        }
      }
    }
    if (cloudy) {
      m.fillCircle(cx - 2, cy + 1, 3, cloud);
      m.fillCircle(cx + 2, cy, 4, cloud);
      m.fillRect(cx - 5, cy + 1, 11, 4, cloud);
    }
    if (rain) {
      const int16_t off = static_cast<int16_t>((now_ms / 150u) % 4u);
      for (int16_t k = 0; k < 3; ++k) {
        m.drawPixel(cx - 3 + k * 3, cy + 6 + off, 0x04DF);
      }
    }
  }

  cards::Deck m_deck;
};

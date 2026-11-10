// artemis scene (FR-21.7) — NASA Artemis Moon-program tracker.
//
// HA pulls the Artemis entries from Launch Library 2 (same source as
// launch_countdown) and pushes observatory/artemis: the last flown
// mission + outcome, the next one + NET date, and a short mission
// description. Cards: NEXT → LAST → description (word-wrapped across
// up to two cards) → two fixed program facts. Visual: an Orion dot on
// a free-return loop around a small Moon in the right-hand column.

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "feeds.h"
#include "fixed_point.h"
#include "scene.h"
#include "theme.h"
#include "time_of_day.h"

class ArtemisScene : public Scene {
 public:
  const char* name() const override { return "artemis"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    matrix.fillScreen(0x0000);
    draw_moon_loop(matrix, now_ms);
    cards::header(matrix, "ART", now_ms);

    feeds::Artemis a{};
    const bool fresh = feeds::get(now_ms, &a);
    char desc[6][cards::kLineCap];
    const uint8_t desc_lines = fresh ? cards::wrap(a.desc, 13, desc, 6) : 0;
    const uint8_t desc_cards = static_cast<uint8_t>((desc_lines + 2) / 3);
    const uint8_t live  = fresh ? static_cast<uint8_t>(2 + desc_cards) : 0;
    const uint8_t count = static_cast<uint8_t>(live + 2);

    cards::Card c;
    cards::clear(c);
    const uint8_t i = m_deck.index() % count;
    const uint16_t v  = theme::ink(theme::Ink::VALUE);
    const uint16_t in = theme::ink(theme::Ink::STATUS_INFO);
    const uint16_t wn = theme::ink(theme::Ink::STATUS_WARN);
    const tod::Reading r = tod::now(now_ms);
    if (i == 0 && fresh) {
      cards::set(c, 0, wn, "NEXT:");
      cards::set(c, 1, v, "%s", a.next);
      fmt_when(c, 2, in, a.next_local_epoch, a.next_estimate, r);
    } else if (i == 1 && fresh) {
      cards::set(c, 0, wn, "LAST:");
      cards::set(c, 1, v, "%s", a.last);
      int16_t yr; uint8_t mo, d, dow;
      tod::date_from_local_epoch(a.last_local_epoch, &yr, &mo, &d, &dow);
      cards::set(c, 2, in, "%s %d %s", tod::month_abbrev(mo), yr,
                 a.last_result == 1 ? "OK" : (a.last_result == 0 ? "FAIL" : "PART"));
    } else if (i < live) {
      const uint8_t base = static_cast<uint8_t>((i - 2) * 3);
      for (uint8_t k = 0; k < 3 && base + k < desc_lines; ++k) {
        cards::set(c, k, k == 0 ? v : in, "%s", desc[base + k]);
      }
    } else if (i == live) {
      cards::set(c, 0, v, "SLS ROCKET");
      cards::set(c, 1, in, "98 M TALL");
      cards::set(c, 2, wn, "8.8M LB THRUST");
    } else {
      cards::set(c, 0, v, "TO THE MOON");
      cards::set(c, 1, in, "384,400 KM");
      cards::set(c, 2, wn, "~3 DAY TRIP");
    }
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), count);
    cards::draw(matrix, c, e, 2, /*backdrop=*/true);
  }

 private:
  static void fmt_when(cards::Card& c, uint8_t row, uint16_t ink, int32_t epoch,
                       bool estimate, const tod::Reading& r) {
    int16_t yr; uint8_t mo, d, dow;
    tod::date_from_local_epoch(epoch, &yr, &mo, &d, &dow);
    if (estimate || !r.valid) {
      cards::set(c, row, ink, "NET %s %d", tod::month_abbrev(mo), yr);
      return;
    }
    const int32_t days = (epoch - r.local_epoch) / 86400;
    if (days > 0) cards::set(c, row, ink, "IN %ld DAYS", static_cast<long>(days));
    else          cards::set(c, row, ink, "%s %u", tod::month_abbrev(mo), static_cast<unsigned>(d));
  }

  // Moon (r=4) at (57,21); Orion traces an ellipse that swings behind it.
  static void draw_moon_loop(Adafruit_Protomatter& m, uint32_t now_ms) {
    constexpr int16_t mx = 57, my = 21;
    m.fillCircle(mx, my, 4, theme::ink(theme::Ink::LABEL));
    m.drawPixel(mx - 1, my - 1, theme::ink(theme::Ink::STATUS_DIM));
    m.drawPixel(mx + 1, my + 2, theme::ink(theme::Ink::STATUS_DIM));
    const uint8_t ang = static_cast<uint8_t>(now_ms / 30u);
    const int16_t ox = static_cast<int16_t>(mx - 2 + (fp::cos_q8(ang) * 7) / 256);
    const int16_t oy = static_cast<int16_t>(my + (fp::sin_q8(ang) * 5) / 256);
    const bool behind = (ox - mx) * (ox - mx) + (oy - my) * (oy - my) <= 16;
    if (!behind) m.drawPixel(ox, oy, theme::ink(theme::Ink::ACCENT));
  }

  cards::Deck m_deck;
};

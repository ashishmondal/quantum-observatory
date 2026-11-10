// hail_mary scene (FR-21.8) — Project Hail Mary easter eggs.
//
// For the viewer who loves the book: short fan-reference cards mixing
// the real astronomy behind the story (Tau Ceti and 40 Eridani are
// real, nearby stars) with nods to the fiction. Text is paraphrase /
// 2–3 word catchphrases only — no passages from the novel.
//
// Visual: the "Petrova line" — a faint infrared-red arc leaving a
// small star at lower left, with Astrophage specks drifting along it.

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "scene.h"
#include "theme.h"

namespace hail_mary_detail {

struct Egg { const char* a; const char* b; const char* c; };

inline constexpr Egg kEggs[] = {
  { "TAU CETI",     "11.9 LY AWAY",  "A REAL STAR!" },
  { "40 ERIDANI",   "16.3 LY AWAY",  "ROCKY'S SUN" },
  { "ASTROPHAGE",   "EATS STARLIGHT", "STORES ENERGY" },
  { "PETROVA LINE", "INFRARED GLOW",  "25.98 MICRONS" },
  { "XENONITE",     "ERIDIAN BUILD",  "SUPER STRONG" },
  { "ROCKY SAYS",   "AMAZE AMAZE",    "AMAZE!" },
  { "ROCKY SAYS",   "FIST MY BUMP",   "" },
  { "TAU CETI IS",  "IN CETUS",       "LOOK SOUTH FALL" },
};
inline constexpr uint8_t kCount = sizeof(kEggs) / sizeof(kEggs[0]);

}  // namespace hail_mary_detail

class HailMaryScene : public Scene {
 public:
  const char* name() const override { return "hail_mary"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_deck.reset();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    using namespace hail_mary_detail;
    matrix.fillScreen(0x0000);
    draw_petrova(matrix, now_ms);
    cards::header(matrix, "PHM", now_ms);

    const Egg& g = kEggs[m_deck.index()];
    cards::Card c;
    cards::set(c, 0, theme::ink(theme::Ink::VALUE), "%s", g.a);
    cards::set(c, 1, theme::ink(theme::Ink::STATUS_INFO), "%s", g.b);
    cards::set(c, 2, theme::ink(theme::Ink::ACCENT_MAGENTA), "%s", g.c);
    const uint32_t e = m_deck.step(now_ms, cards::duration_ms(c), kCount);
    cards::draw(matrix, c, e, 2, /*backdrop=*/true);
  }

  // Shared with the HAIL_MARY theme's clock background so both draw the
  // same arc. Petrova red at H≈345°; V follows a travelling pulse.
  static void draw_petrova(Adafruit_Protomatter& m, uint32_t now_ms) {
    m.fillCircle(3, 28, 2, 0x8400);   // dim star
    const int16_t pulse = static_cast<int16_t>((now_ms / 30u) % 90u);
    for (int16_t x = 4; x < PANEL_WIDTH; ++x) {
      const int16_t y = static_cast<int16_t>(28 - ((x - 4) * (x - 4)) / 190);
      int16_t d = x - pulse;
      if (d < 0) d = -d;
      const uint8_t v = static_cast<uint8_t>(d < 8 ? 18 - d * 2 : 4);  // 5-bit red
      m.drawPixel(x, y, static_cast<uint16_t>((v << 11) | (v / 6)));
    }
    // Astrophage specks riding the arc at three speeds.
    for (uint8_t k = 0; k < 6; ++k) {
      const int16_t x = static_cast<int16_t>(
          4 + ((now_ms / (45u + k * 13u)) + k * 11u) % (PANEL_WIDTH - 4));
      const int16_t y = static_cast<int16_t>(28 - ((x - 4) * (x - 4)) / 190 - 1 + (k & 1) * 2);
      m.drawPixel(x, y, 0x6001);
    }
  }

 private:
  cards::Deck m_deck;
};

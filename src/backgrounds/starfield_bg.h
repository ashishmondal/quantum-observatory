// Static deep-sky starfield background, with a small twinkling overlay.
//
// The bulk of the field is generated once at init() from a fixed RNG
// seed and redrawn unchanged every frame — cheap and rock-steady.
// On top of that, a small fixed pool of TWINKLE stars cycles through
// random positions / hues / lifetimes so a few sparkles are always
// fading in or out somewhere on the panel.
//
// Visual model (matches the reference image):
//   - Very dim navy background fill (palette NIGHT_SKY).
//   - Sparse "dust" pixels — peripheral-vision texture only.
//   - A layer of slightly brighter field stars (single bright pixel).
//   - A small population of "hero" stars rendered as a + with bright
//     center, mid-bright cardinal arms, and faint outer arm tips —
//     reads as a diffraction-spike sparkle.
//   - A handful of TWINKLE stars on top, with trapezoidal fade-in/out
//     and randomized rebirth elsewhere on the panel.
//   - Hue is heavily weighted to white; blue and amber are accents.

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "color_palette.h"
#include "config.h"

class StarfieldBg {
public:
  void init() {
    uint32_t s = 0xC0FFEEu;
    auto next = [&s]() -> uint32_t {
      s = s * 1664525u + 1013904223u;
      return s;
    };

    int idx = 0;
    auto place = [&](StarClass cls) {
      // Reject overlapping bright stars so heroes don't merge into a
      // mush — only check against earlier brights, dust can land
      // anywhere (the eye won't notice 2 dim pixels touching).
      for (int tries = 0; tries < 8; ++tries) {
        const uint8_t x = static_cast<uint8_t>(next() % PANEL_WIDTH);
        const uint8_t y = static_cast<uint8_t>(next() % PANEL_HEIGHT);
        if (cls >= StarClass::MEDIUM) {
          bool too_close = false;
          for (int j = 0; j < idx; ++j) {
            if (m_class[j] < StarClass::MEDIUM) continue;
            const int dx = static_cast<int>(m_x[j]) - x;
            const int dy = static_cast<int>(m_y[j]) - y;
            if (dx * dx + dy * dy < 16) { too_close = true; break; }
          }
          if (too_close) continue;
        }
        // Hue distribution: mostly white, blue accent, rare amber.
        const uint8_t h = static_cast<uint8_t>(next() & 0xFF);
        Hue hue;
        if      (h < 220) hue = Hue::WHITE;
        else if (h < 245) hue = Hue::BLUE;
        else              hue = Hue::AMBER;

        m_x[idx]     = x;
        m_y[idx]     = y;
        m_class[idx] = cls;
        m_hue[idx]   = hue;
        ++idx;
        return;
      }
      // Couldn't place after 8 tries — give up on this star, leave the
      // slot unused. We'll trim NUM_STARS reporting to `idx` at draw.
    };

    for (int i = 0; i < NUM_DUST;   ++i) place(StarClass::DUST);
    for (int i = 0; i < NUM_SMALL;  ++i) place(StarClass::SMALL);
    for (int i = 0; i < NUM_MEDIUM; ++i) place(StarClass::MEDIUM);
    for (int i = 0; i < NUM_HERO;   ++i) place(StarClass::HERO);
    m_count = static_cast<uint16_t>(idx);

    // Cache the night-sky fill color so render() doesn't have to look it
    // up per frame. Index 28 (out of 0..63) gives a barely-visible navy
    // wash — enough to keep the panel from reading as pure black, not
    // enough to wash out the dust layer.
    m_bg_color = palette::fg(palette::Id::NIGHT_SKY, 28);

    // Seed the twinkle RNG separately so changing twinkle counts
    // doesn't shift the static field's layout.
    m_twinkle_rng = 0xDEADBEEFu;
    for (int i = 0; i < NUM_TWINKLE; ++i) {
      twinkle_spawn(i, /*now_ms=*/0);
      // Stagger initial births into the past so they don't all
      // fade in together on the first frame.
      m_tw_birth[i] = 0u - (twinkle_rand() % m_tw_life[i]);
    }
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    matrix.fillScreen(m_bg_color);

    for (uint16_t i = 0; i < m_count; ++i) {
      const palette::Id pal = palette_for_hue(m_hue[i]);
      const int16_t cx = m_x[i];
      const int16_t cy = m_y[i];

      // Per-class brightness profile. Tuned by eye against the
      // reference image; if you bump these you almost certainly want
      // to bump the others proportionally so the visual "weight"
      // hierarchy survives.
      uint8_t br_center, br_arm, br_tip;
      switch (m_class[i]) {
        case StarClass::DUST:   br_center =  6; br_arm = 0; br_tip = 0; break;
        case StarClass::SMALL:  br_center = 18; br_arm = 0; br_tip = 0; break;
        case StarClass::MEDIUM: br_center = 36; br_arm = 5; br_tip = 0; break;
        default: /* HERO */     br_center = 63; br_arm = 16; br_tip = 4; break;
      }

      matrix.drawPixel(cx, cy, palette::fg(pal, br_center));

      if (br_arm) {
        const uint16_t c = palette::fg(pal, br_arm);
        if (cx - 1 >= 0)            matrix.drawPixel(cx - 1, cy, c);
        if (cx + 1 < PANEL_WIDTH)   matrix.drawPixel(cx + 1, cy, c);
        if (cy - 1 >= 0)            matrix.drawPixel(cx, cy - 1, c);
        if (cy + 1 < PANEL_HEIGHT)  matrix.drawPixel(cx, cy + 1, c);
      }
      if (br_tip) {
        const uint16_t c = palette::fg(pal, br_tip);
        if (cx - 2 >= 0)            matrix.drawPixel(cx - 2, cy, c);
        if (cx + 2 < PANEL_WIDTH)   matrix.drawPixel(cx + 2, cy, c);
        if (cy - 2 >= 0)            matrix.drawPixel(cx, cy - 2, c);
        if (cy + 2 < PANEL_HEIGHT)  matrix.drawPixel(cx, cy + 2, c);
      }
    }

    // --- Twinkle overlay -------------------------------------------
    // A small pool of stars with trapezoidal envelope fades in/out.
    // On expiry, respawn elsewhere so the eye keeps catching new
    // sparkles in different parts of the panel.
    for (int i = 0; i < NUM_TWINKLE; ++i) {
      const uint32_t age  = now_ms - m_tw_birth[i];   // unsigned wrap-safe
      const uint32_t life = m_tw_life[i];
      if (age >= life) {
        twinkle_spawn(i, now_ms);
        continue;
      }
      const uint8_t env = twinkle_envelope(age, life);  // 0..255
      if (env == 0) continue;

      // Envelope (0..255) -> 6-bit FG index. The high end (HERO has
      // br_center=63) sets the ceiling — we cap at 48 so twinkles
      // visually sit just below the brightest static heroes.
      const uint8_t br = static_cast<uint8_t>((env * 48u) >> 8);
      if (br == 0) continue;

      const palette::Id pal = palette_for_hue(static_cast<Hue>(m_tw_hue[i]));
      const int16_t cx = m_tw_x[i];
      const int16_t cy = m_tw_y[i];
      matrix.drawPixel(cx, cy, palette::fg(pal, br));

      // Faint cardinal arms only when the twinkle is near peak —
      // gives it a moment of "presence" mid-life without being a
      // permanent + sparkle.
      if (br >= 24) {
        const uint8_t arm_b = static_cast<uint8_t>(br >> 2);
        const uint16_t c = palette::fg(pal, arm_b);
        if (cx - 1 >= 0)           matrix.drawPixel(cx - 1, cy, c);
        if (cx + 1 < PANEL_WIDTH)  matrix.drawPixel(cx + 1, cy, c);
        if (cy - 1 >= 0)           matrix.drawPixel(cx, cy - 1, c);
        if (cy + 1 < PANEL_HEIGHT) matrix.drawPixel(cx, cy + 1, c);
      }
    }
  }

private:
  enum class StarClass : uint8_t { DUST = 0, SMALL = 1, MEDIUM = 2, HERO = 3 };
  enum class Hue       : uint8_t { WHITE = 0, BLUE = 1, AMBER = 2 };

  static palette::Id palette_for_hue(Hue h) {
    switch (h) {
      case Hue::BLUE:  return palette::Id::STAR_BLUE;
      case Hue::AMBER: return palette::Id::STAR_AMBER;
      default:         return palette::Id::STAR_WHITE;
    }
  }

  // Population counts. Tuned by eye on the actual panel:
  //   dust ~5%  pixel coverage — visible texture without crowding.
  static constexpr int NUM_DUST    = 90;
  static constexpr int NUM_SMALL   = 28;
  static constexpr int NUM_MEDIUM  = 10;
  static constexpr int NUM_HERO    = 4;
  static constexpr int NUM_TWINKLE = 6;   // moving sparkles on top
  static constexpr int CAPACITY    = NUM_DUST + NUM_SMALL + NUM_MEDIUM + NUM_HERO;

  // Twinkle lifetime range, in milliseconds. Long enough that the
  // fade is graceful, short enough that the eye keeps catching new
  // sparkles.
  static constexpr uint32_t TW_LIFE_MIN_MS = 1800;
  static constexpr uint32_t TW_LIFE_MAX_MS = 5500;

  uint8_t   m_x[CAPACITY]{};
  uint8_t   m_y[CAPACITY]{};
  StarClass m_class[CAPACITY]{};
  Hue       m_hue[CAPACITY]{};
  uint16_t  m_count   = 0;
  uint16_t  m_bg_color = 0;

  // Twinkle layer state. Separate RNG from the static field so that
  // tweaking NUM_TWINKLE doesn't shift the static layout.
  uint32_t  m_twinkle_rng = 0xDEADBEEFu;
  uint8_t   m_tw_x[NUM_TWINKLE]{};
  uint8_t   m_tw_y[NUM_TWINKLE]{};
  uint8_t   m_tw_hue[NUM_TWINKLE]{};
  uint32_t  m_tw_birth[NUM_TWINKLE]{};
  uint32_t  m_tw_life[NUM_TWINKLE]{};

  uint32_t twinkle_rand() {
    m_twinkle_rng = m_twinkle_rng * 1664525u + 1013904223u;
    return m_twinkle_rng;
  }

  void twinkle_spawn(int i, uint32_t now_ms) {
    m_tw_x[i] = static_cast<uint8_t>(twinkle_rand() % PANEL_WIDTH);
    m_tw_y[i] = static_cast<uint8_t>(twinkle_rand() % PANEL_HEIGHT);
    const uint8_t h = static_cast<uint8_t>(twinkle_rand() & 0xFF);
    if      (h < 200) m_tw_hue[i] = static_cast<uint8_t>(Hue::WHITE);
    else if (h < 240) m_tw_hue[i] = static_cast<uint8_t>(Hue::BLUE);
    else              m_tw_hue[i] = static_cast<uint8_t>(Hue::AMBER);
    m_tw_birth[i] = now_ms;
    m_tw_life[i]  = TW_LIFE_MIN_MS
                  + (twinkle_rand() % (TW_LIFE_MAX_MS - TW_LIFE_MIN_MS));
  }

  // Trapezoidal envelope: 0 -> 255 -> 255 -> 0.
  // Fade-in 25%, plateau 25%, fade-out 50% — the long fade-out reads
  // most like a real twinkle dying away.
  static uint8_t twinkle_envelope(uint32_t age, uint32_t life) {
    const uint32_t fi_end  = life / 4;             // 0..25%
    const uint32_t pl_end  = life / 2;             // 25..50%
    if (age < fi_end) {
      return static_cast<uint8_t>((age * 255u) / fi_end);
    }
    if (age < pl_end) {
      return 255;
    }
    const uint32_t fo_len = life - pl_end;
    const uint32_t into   = age - pl_end;
    const uint32_t left   = fo_len - into;
    return static_cast<uint8_t>((left * 255u) / fo_len);
  }
};

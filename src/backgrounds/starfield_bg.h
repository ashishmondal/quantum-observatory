// Twinkling-stars background renderer.
//
// Each star has a finite, randomized lifetime. While alive it fades in,
// shimmers, then fades out; on death it is reborn at a fresh random
// location with new size / hue / lifetime. This keeps a constant NUM_STARS
// stars on screen but makes the field feel alive rather than static.
//
// Stars are drawn as a "+" with a bright center and dimmer arms that fall
// off radially.

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "fixed_point.h"

class StarfieldBg {
public:
  void init() {
    m_rng = 0xC0FFEEu;
    // Stagger initial births in the past so stars are at different points
    // in their lifecycle on the very first frame (no synchronized fade-in).
    for (int i = 0; i < NUM_STARS; ++i) {
      spawn(i, /*now_ms=*/0);
      // Pretend each star was born up to one full lifetime ago, so some
      // are already mid-life or about to die.
      const uint32_t back = rand_u32() % m_life[i];
      m_birth[i] = 0u - back;  // wraps; arithmetic with now_ms still works
    }
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    matrix.fillScreen(0x0000);

    for (int i = 0; i < NUM_STARS; ++i) {
      const uint32_t age = now_ms - m_birth[i];  // unsigned wrap-safe
      const uint32_t life = m_life[i];

      if (age >= life) {
        spawn(i, now_ms);
        // Skip drawing this frame; new star will appear next frame.
        continue;
      }

      // Lifecycle envelope: fade in over first 20% of life, fade out over
      // last 30%, full envelope = 1.0 in the middle. Returns 0..255.
      const uint8_t env = envelope(age, life);
      if (env == 0) continue;

      // Shimmer: gentle sine modulation on top of the envelope so the
      // star isn't a flat brightness. Scale ~0.65..1.0 of envelope.
      const uint8_t shimmer_angle =
          static_cast<uint8_t>((now_ms >> 5) * (1u + (m_speed[i] & 0x3))
                               + m_phase[i]);
      // sin_q8: -256..+256 → map to 168..256, then * env / 256.
      const int16_t s = fp::sin_q8(shimmer_angle);  // -256..256
      const int32_t mod = 212 + (s >> 2);            // ~168..256
      int32_t br32 = (static_cast<int32_t>(env) * mod) >> 8;
      if (br32 > 255) br32 = 255;
      if (br32 < 0)   br32 = 0;
      // Gamma-correct (~2.0) so low brightnesses fade smoothly instead of
      // dropping off in chunky steps at our limited bit depth. Cheap:
      // perceptual b' = b*b / 255.
      const uint8_t br =
          static_cast<uint8_t>((br32 * br32 + 127) / 255);

      const uint8_t size = static_cast<uint8_t>(m_pack[i] & 0x3);
      const uint8_t hue  = static_cast<uint8_t>((m_pack[i] >> 2) & 0x3);

      const int16_t cx = m_x[i];
      const int16_t cy = m_y[i];

      matrix.drawPixel(cx, cy, color_for(br, hue));
      if (size == 0) continue;

      // Inner arms: ~1/4 of center brightness.
      {
        const uint8_t arm_b = static_cast<uint8_t>(br >> 2);
        const uint16_t arm_c = color_for(arm_b, hue);
        if (cx - 1 >= 0)            matrix.drawPixel(cx - 1, cy, arm_c);
        if (cx + 1 < PANEL_WIDTH)   matrix.drawPixel(cx + 1, cy, arm_c);
        if (cy - 1 >= 0)            matrix.drawPixel(cx, cy - 1, arm_c);
        if (cy + 1 < PANEL_HEIGHT)  matrix.drawPixel(cx, cy + 1, arm_c);
      }

      // Outer arm tips on the largest stars — ~1/8.
      if (size >= 2) {
        const uint8_t tip_b = static_cast<uint8_t>(br >> 3);
        const uint16_t tip_c = color_for(tip_b, hue);
        if (cx - 2 >= 0)            matrix.drawPixel(cx - 2, cy, tip_c);
        if (cx + 2 < PANEL_WIDTH)   matrix.drawPixel(cx + 2, cy, tip_c);
        if (cy - 2 >= 0)            matrix.drawPixel(cx, cy - 2, tip_c);
        if (cy + 2 < PANEL_HEIGHT)  matrix.drawPixel(cx, cy + 2, tip_c);
      }
    }
  }

private:
  static constexpr int NUM_STARS = 20;

  // Lifetime range, in milliseconds.
  static constexpr uint32_t LIFE_MIN_MS = 2500;
  static constexpr uint32_t LIFE_MAX_MS = 9000;

  // Per-star RNG (LCG, plenty random for visual variety).
  uint32_t m_rng = 0xC0FFEEu;
  uint32_t rand_u32() {
    m_rng = m_rng * 1664525u + 1013904223u;
    return m_rng;
  }

  // (Re)birth star i at a fresh random location with new attributes.
  void spawn(int i, uint32_t now_ms) {
    m_x[i] = static_cast<uint8_t>(rand_u32() % PANEL_WIDTH);
    m_y[i] = static_cast<uint8_t>(rand_u32() % PANEL_HEIGHT);
    m_phase[i] = static_cast<uint8_t>(rand_u32());
    m_speed[i] = static_cast<uint8_t>(rand_u32());

    const uint8_t r = static_cast<uint8_t>(rand_u32() & 0xFF);
    uint8_t size;
    if      (r < 140) size = 0;  // small dot
    else if (r < 230) size = 1;  // 1-pixel arms
    else              size = 2;  // 2-pixel arms (rare, prominent)

    const uint8_t h = static_cast<uint8_t>(rand_u32() & 0xFF);
    uint8_t hue;
    if      (h < 170) hue = 0;   // white
    else if (h < 230) hue = 1;   // cool
    else              hue = 2;   // warm

    m_pack[i] = static_cast<uint8_t>((size & 0x3) | ((hue & 0x3) << 2));

    m_birth[i] = now_ms;
    m_life[i]  = LIFE_MIN_MS + (rand_u32() % (LIFE_MAX_MS - LIFE_MIN_MS));
  }

  // Trapezoidal lifecycle envelope: 0 → 255 → 255 → 0 over life.
  // Fade-in: first 20%. Fade-out: last 30%. Plateau in between.
  static uint8_t envelope(uint32_t age, uint32_t life) {
    const uint32_t fade_in_end  = life / 5;          // 20%
    const uint32_t fade_out_beg = life - (life * 3 / 10);  // 70%
    if (age < fade_in_end) {
      return static_cast<uint8_t>((age * 255) / fade_in_end);
    }
    if (age < fade_out_beg) {
      return 255;
    }
    const uint32_t fade_out_len = life - fade_out_beg;
    const uint32_t into_fade    = age - fade_out_beg;
    const uint32_t left = fade_out_len - into_fade;
    return static_cast<uint8_t>((left * 255) / fade_out_len);
  }

  // RGB565 colour from an 8-bit brightness and a hue index.
  // hue 0 = white, 1 = cool (cyan-ish), 2 = warm (amber-ish).
  static uint16_t color_for(uint8_t b, uint8_t hue) {
    uint16_t r5, g6, b5;
    switch (hue) {
      case 1:  // cool / cyan-white
        r5 = static_cast<uint16_t>((b >> 4));
        g6 = static_cast<uint16_t>((b >> 2));
        b5 = static_cast<uint16_t>((b >> 3));
        break;
      case 2:  // warm / amber-white
        r5 = static_cast<uint16_t>((b >> 3));
        g6 = static_cast<uint16_t>((b >> 3));
        b5 = static_cast<uint16_t>((b >> 5));
        break;
      default: // neutral white
        r5 = static_cast<uint16_t>((b >> 3));
        g6 = static_cast<uint16_t>((b >> 2));
        b5 = static_cast<uint16_t>((b >> 3));
        break;
    }
    if (r5 > 0x1F) r5 = 0x1F;
    if (g6 > 0x3F) g6 = 0x3F;
    if (b5 > 0x1F) b5 = 0x1F;
    return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
  }

  uint8_t  m_x[NUM_STARS]{};
  uint8_t  m_y[NUM_STARS]{};
  uint8_t  m_phase[NUM_STARS]{};
  uint8_t  m_speed[NUM_STARS]{};
  // Low 2 bits = size (0..2), next 2 bits = hue (0..2).
  uint8_t  m_pack[NUM_STARS]{};
  uint32_t m_birth[NUM_STARS]{};   // ms timestamp of spawn
  uint32_t m_life[NUM_STARS]{};    // total lifetime in ms
};

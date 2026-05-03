// 3-level parallax starfield background renderer (extracted from phase 2.3
// scene). Pure renderer: no Scene interface, no matrix.show().
//
// (added in phase 2.5 — refactor of parallax_starfield_scene.h)

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "fixed_point.h"

class ParallaxBg {
public:
  void init() {
    uint32_t s = 0xBADC0DEu;
    auto next = [&s]() -> uint32_t {
      s = s * 1664525u + 1013904223u;
      return s;
    };

    int idx = 0;
    for (int layer = 0; layer < NUM_LAYERS; ++layer) {
      for (int i = 0; i < STARS_PER_LAYER[layer]; ++i, ++idx) {
        m_x[idx]     = static_cast<int16_t>((next() % PANEL_WIDTH) << fp::Q8_SHIFT);
        m_y[idx]     = static_cast<uint8_t>(next() % PANEL_HEIGHT);
        m_layer[idx] = static_cast<uint8_t>(layer);
      }
    }
    m_last_ms = 0;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    uint32_t dt = now_ms - m_last_ms;
    if (dt > 100u) dt = 100u;
    m_last_ms = now_ms;

    int16_t step_q8[NUM_LAYERS];
    for (int l = 0; l < NUM_LAYERS; ++l) {
      step_q8[l] = static_cast<int16_t>(
          (static_cast<int32_t>(SPEED_Q8[l]) * static_cast<int32_t>(dt)) >> 4);
    }

    matrix.fillScreen(0x0000);

    constexpr int16_t WIDTH_Q8 = static_cast<int16_t>(PANEL_WIDTH << fp::Q8_SHIFT);

    for (int i = 0; i < TOTAL_STARS; ++i) {
      const uint8_t l = m_layer[i];

      int32_t x = static_cast<int32_t>(m_x[i]) + step_q8[l];
      if (x >= WIDTH_Q8) x -= WIDTH_Q8;
      if (x <  0)        x += WIDTH_Q8;
      m_x[i] = static_cast<int16_t>(x);

      const int16_t  px   = static_cast<int16_t>(x >> fp::Q8_SHIFT);
      const uint8_t  frac = static_cast<uint8_t>(x & 0xFF);
      const uint16_t base = LAYER_COLOR[l];
      const uint8_t  y    = m_y[i];

      const int16_t px2 = (px + 1) % PANEL_WIDTH;
      matrix.drawPixel(px,  y, scale_rgb565(base, 255 - frac));
      matrix.drawPixel(px2, y, scale_rgb565(base, frac));

      const int len = TRAIL_LEN[l];
      for (int k = 1; k <= len; ++k) {
        const uint8_t w = static_cast<uint8_t>((255 * (len - k)) / len);
        if (w == 0) break;
        int16_t tx = px - k;
        if (tx < 0) tx += PANEL_WIDTH;
        matrix.drawPixel(tx, y, scale_rgb565(base, w));
      }
    }
  }

private:
  static uint16_t scale_rgb565(uint16_t c, uint8_t w) {
    const uint16_t r = (c >> 11) & 0x1F;
    const uint16_t g = (c >>  5) & 0x3F;
    const uint16_t b =  c        & 0x1F;
    const uint16_t rs = (r * w) >> 8;
    const uint16_t gs = (g * w) >> 8;
    const uint16_t bs = (b * w) >> 8;
    return static_cast<uint16_t>((rs << 11) | (gs << 5) | bs);
  }

  static constexpr int  NUM_LAYERS = 3;
  static constexpr int  STARS_PER_LAYER[NUM_LAYERS] = {18, 12, 6};
  static constexpr int  TOTAL_STARS = 18 + 12 + 6;
  static constexpr int16_t SPEED_Q8[NUM_LAYERS]   = {64, 128, 256};
  static constexpr int     TRAIL_LEN[NUM_LAYERS]  = {2, 4, 8};
  static constexpr uint16_t LAYER_COLOR[NUM_LAYERS] = {
      0x4A49, 0x9CD3, 0xFFFF,
  };

  int16_t  m_x[TOTAL_STARS]{};
  uint8_t  m_y[TOTAL_STARS]{};
  uint8_t  m_layer[TOTAL_STARS]{};
  uint32_t m_last_ms = 0;
};

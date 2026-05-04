// Animated nebula background renderer (extracted from phase 2.4 scene).
// Pure renderer: no Scene interface, no matrix.show().
//
// (added in phase 2.5 — refactor of nebula_scene.h)

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "color_palette.h"
#include "config.h"
#include "fixed_point.h"

class NebulaBg {
public:
  void init() {
    uint32_t s = 0xFEEDFACEu;
    auto next = [&s]() -> uint32_t {
      s = s * 1664525u + 1013904223u;
      return s;
    };

    for (int gy = 0; gy < GRID_H; ++gy) {
      for (int gx = 0; gx < GRID_W; ++gx) {
        const int idx = gy * GRID_W + gx;
        m_base[idx]  = static_cast<uint8_t>(32 + (next() % 192));
        m_phase[idx] = static_cast<uint8_t>(next());
      }
    }
    m_last_ms = 0;
    m_t       = 0;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    uint32_t dt = now_ms - m_last_ms;
    if (dt > 100u) dt = 100u;
    m_last_ms = now_ms;
    m_t += static_cast<uint8_t>(dt >> 3);

    // Palette-cycle phase: walks the 192-entry NEBULA_CLOUDS BG ramp,
    // wrapping seamlessly because the ramp's last stop matches its first.
    // (dt >> 4) is ~6 steps/sec at 100 ms ticks — slow, dreamy drift.
    m_palette_shift += (dt >> 4);

    uint8_t cell[GRID_W * GRID_H];
    for (int i = 0; i < GRID_W * GRID_H; ++i) {
      const int16_t s = fp::sin_q8(static_cast<uint8_t>(m_phase[i] + m_t));
      int v = m_base[i] + ((s * 112) >> 8);
      if (v < 0)   v = 0;
      if (v > 255) v = 255;
      cell[i] = static_cast<uint8_t>(v);
    }

    constexpr int CELL_W = PANEL_WIDTH  / GRID_W;
    constexpr int CELL_H = PANEL_HEIGHT / GRID_H;

    for (int py = 0; py < PANEL_HEIGHT; ++py) {
      const int gy0 = py / CELL_H;
      const int gy1 = (gy0 + 1 < GRID_H) ? gy0 + 1 : gy0;
      const int fy  = py & (CELL_H - 1);
      const int wy1 = fy;
      const int wy0 = CELL_H - fy;

      for (int px = 0; px < PANEL_WIDTH; ++px) {
        const int gx0 = px / CELL_W;
        const int gx1 = (gx0 + 1 < GRID_W) ? gx0 + 1 : gx0;
        const int fx  = px & (CELL_W - 1);
        const int wx1 = fx;
        const int wx0 = CELL_W - fx;

        const uint8_t v00 = cell[gy0 * GRID_W + gx0];
        const uint8_t v10 = cell[gy0 * GRID_W + gx1];
        const uint8_t v01 = cell[gy1 * GRID_W + gx0];
        const uint8_t v11 = cell[gy1 * GRID_W + gx1];

        const int top = v00 * wx0 + v10 * wx1;
        const int bot = v01 * wx0 + v11 * wx1;
        const int v   = (top * wy0 + bot * wy1) >> 4;

        // Map intensity 0..255 → BG ramp index 0..191, then offset by
        // the global palette shift. Animation is FREE: the per-cell
        // bilinear pattern stays the same frame-to-frame, only the
        // palette cycles, but the eye perceives flowing color motion.
        const uint16_t idx = static_cast<uint16_t>((v * (palette::BG_LEN - 1)) >> 8);
        matrix.drawPixel(px, py,
            palette::bg(palette::Id::NEBULA_CLOUDS, idx, m_palette_shift));
      }
    }
  }

private:
  static constexpr int GRID_W = 16;
  static constexpr int GRID_H = 8;

  uint8_t  m_base[GRID_W * GRID_H]{};
  uint8_t  m_phase[GRID_W * GRID_H]{};
  uint8_t  m_t       = 0;
  uint32_t m_last_ms = 0;
  uint16_t m_palette_shift = 0;
};

// Graphics smoke-test scene.
//
// One screen that exercises every pixel pipeline we care about and
// reports its own frame rate so any regression is visible at a glance.
// Brought up over MQTT with payload {"scene_id":"gfx_test"} on
// observatory/scene.
//
// Layout (64x32 panel, no clock chrome — wants_clock_chrome() = false):
//
//     rows  0..9   nebula BG palette cycling across full width.
//                  Confirms palette::bg() seamless wrap (no visible
//                  seam at index 191->0) and that cycling has zero
//                  per-frame allocation cost.
//     rows 10..11  black separator
//     rows 12..13  STAR_WHITE FG ramp, brightness 0..63 across width
//     rows 14..15  STAR_BLUE  FG ramp
//     rows 16..17  STAR_AMBER FG ramp
//                  Confirms palette::fg() smoothness and that 64
//                  brightness steps cover all visible levels at
//                  PANEL_BIT_DEPTH=5.
//     rows 18..19  black separator
//     rows 20..31  Picopixel readout:
//                    FPS NN     — measured live, refreshed once/sec
//                    SHIFT NNNN — current cycle phase
//                    T NNNs     — uptime since this scene was init'd
//
// Self-contained — no globals from main.cpp. Owns its own frame
// counter so it works as a true diagnostic regardless of what other
// instrumentation is in flight.

#pragma once

#include <stdint.h>
#include <stdio.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>

#include "color_palette.h"
#include "config.h"
#include "scenes/scene.h"

class GfxTestScene : public Scene {
public:
  const char* name() const override { return "gfx_test"; }
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    (void)matrix;
    m_init_ms       = 0;
    m_initialised   = false;
    m_last_tick_ms  = 0;
    m_palette_shift = 0;
    m_frames_in_window = 0;
    m_fps_display      = 0;
    m_window_start_ms  = 0;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (!m_initialised) {
      m_init_ms          = now_ms;
      m_last_tick_ms     = now_ms;
      m_window_start_ms  = now_ms;
      m_initialised      = true;
    }

    // --- Palette-cycle phase advance -------------------------------------
    // Wall-clock-driven so the visual speed is consistent across whatever
    // FPS the test is actually achieving (the whole point of the diag).
    const uint32_t dt = now_ms - m_last_tick_ms;
    m_last_tick_ms = now_ms;
    // ~24 steps/sec — fast enough to see motion clearly, slow enough that
    // any frame drop reads as a visible stutter.
    m_palette_shift += static_cast<uint16_t>((dt * 24u) / 1000u);

    // --- FPS sampling ----------------------------------------------------
    m_frames_in_window++;
    const uint32_t window_age = now_ms - m_window_start_ms;
    if (window_age >= 1000u) {
      // (frames * 1000) / window_age — integer math, no divide-by-zero
      // because window_age >= 1000.
      m_fps_display    = (m_frames_in_window * 1000u) / window_age;
      m_frames_in_window = 0;
      m_window_start_ms  = now_ms;
    }

    // --- Draw ------------------------------------------------------------
    matrix.fillScreen(0x0000);

    // Top strip: cycling nebula. Map x in 0..63 → BG index 0..191
    // (each column = 3 palette steps), then add the global shift. The
    // whole strip looks uniform per frame but the colors flow sideways.
    for (int y = 0; y < 10; ++y) {
      for (int x = 0; x < PANEL_WIDTH; ++x) {
        const uint16_t idx = static_cast<uint16_t>(x * 3);  // 0..189
        matrix.drawPixel(x, y,
            palette::bg(palette::Id::NEBULA_CLOUDS, idx, m_palette_shift));
      }
    }

    // FG ramps. Brightness across the panel width — 64 px maps 1:1 onto
    // the 64-entry FG region, so each pixel is its own brightness step.
    draw_fg_ramp(matrix, /*y0=*/12, palette::Id::STAR_WHITE);
    draw_fg_ramp(matrix, /*y0=*/14, palette::Id::STAR_BLUE);
    draw_fg_ramp(matrix, /*y0=*/16, palette::Id::STAR_AMBER);

    // Text readout. Picopixel is ~5 px tall, baseline at the bottom.
    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);
    matrix.setTextColor(0xFFFF);  // white, full brightness — easy to read

    char line[16];

    snprintf(line, sizeof(line), "FPS %lu",
             static_cast<unsigned long>(m_fps_display));
    matrix.setCursor(1, 25);
    matrix.print(line);

    snprintf(line, sizeof(line), "SHIFT %u",
             static_cast<unsigned>(m_palette_shift));
    matrix.setCursor(28, 25);
    matrix.print(line);

    const uint32_t uptime_s = (now_ms - m_init_ms) / 1000u;
    snprintf(line, sizeof(line), "T %lus",
             static_cast<unsigned long>(uptime_s));
    matrix.setCursor(1, 31);
    matrix.print(line);

    // Tiny "moving witness" pixel in the bottom-right that hops 1 px
    // every frame. If FPS feels off but the number says fine, watch the
    // pixel — irregular hopping = jitter, smooth march = real frame loss.
    const uint8_t hop = static_cast<uint8_t>(m_frames_in_window & 0x07);
    matrix.drawPixel(PANEL_WIDTH - 1 - hop, 31, 0xF800);  // bright red
  }

private:
  static void draw_fg_ramp(Adafruit_Protomatter& matrix,
                           int y0, palette::Id pal) {
    for (int x = 0; x < PANEL_WIDTH; ++x) {
      const uint16_t c = palette::fg(pal, static_cast<uint8_t>(x));
      matrix.drawPixel(x, y0,     c);
      matrix.drawPixel(x, y0 + 1, c);
    }
  }

  bool     m_initialised      = false;
  uint32_t m_init_ms          = 0;
  uint32_t m_last_tick_ms     = 0;
  uint16_t m_palette_shift    = 0;

  // Sliding 1 s window FPS sampler. window_start advances every time
  // we close a window, fps_display holds the last completed sample.
  uint32_t m_window_start_ms  = 0;
  uint32_t m_frames_in_window = 0;
  uint32_t m_fps_display      = 0;
};

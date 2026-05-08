// Solid-color seconds counter scene: prints millis()/1000 centered on the
// panel using the built-in Adafruit_GFX 5x7 font (size 2 → 12x16 cells).
// Acts as the first "real" scene, proving text rendering and per-frame
// updates through the Scene interface.
//
// (added in phase 1.4)

#pragma once

#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "scene.h"
#include "theme.h"

class ClockScene : public Scene {
public:
  const char* name() const override { return "clock"; }

  void init(Adafruit_Protomatter& matrix) override {
    // Built-in 5x7 font; size 2 → 12 px wide, 16 px tall per glyph (incl.
    // the 1 px GFX spacing). Background colour stays cyan so we can verify
    // both fill and text in one glance.
    matrix.setFont();              // built-in default — no theme role for built-in
    matrix.setTextSize(2);
    matrix.setTextWrap(false);
    matrix.setTextColor(theme::ink(theme::Ink::ACCENT));
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // Integer-only formatting; %lu matches uint32_t on this core.
    // Buffer holds up to 10 digits + NUL — enough for any uint32_t value.
    char buf[12];
    const uint32_t seconds = now_ms / 1000u;
    const int len = snprintf(buf, sizeof(buf), "%lu",
                             static_cast<unsigned long>(seconds));

    // Each size-2 glyph occupies 12 px horizontally (6 px font * 2). Compute
    // the centred origin as integer math (NFR-1.3). If the string ever grows
    // wider than the panel (uptime past ~5 digits at size 2), clamp left.
    constexpr int GLYPH_W = 6 * 2;
    constexpr int GLYPH_H = 8 * 2;
    int x = (PANEL_WIDTH  - len * GLYPH_W) / 2;
    int y = (PANEL_HEIGHT - GLYPH_H) / 2;
    if (x < 0) x = 0;

    matrix.fillScreen(0x0000);  // universal background
    matrix.setCursor(x, y);
    matrix.print(buf);
    // matrix.show() is called by loop1() after chrome (phase 3.5.2).
  }
};

// Boot scene: branded "OBS" splash, centered on a black field.
// Matches the `boot` entry in REQUIREMENTS.md §6 (shown at startup; real
// dispatch happens in Phase 5). Static — re-renders the same frame each
// tick. Cheap and harmless to call every loop().
//
// (added in phase 1.5)

#pragma once

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "scene.h"
#include "theme.h"

class BootScene : public Scene {
public:
  const char* name() const override { return "boot"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setFont();              // built-in 5x7 — no theme role for built-in
    matrix.setTextSize(1);
    matrix.setTextWrap(false);
    matrix.setTextColor(theme::ink(theme::Ink::ACCENT));
  }

  void render(Adafruit_Protomatter& matrix, uint32_t /*now_ms*/) override {
    // Built-in GFX font glyph cell at size 1 is 6 px wide x 8 px tall (5x7
    // glyph + 1 px advance / descender). Compute centring with integer
    // math (NFR-1.3). String length is a compile-time constant.
    static const char kLabel[] = "OBS";
    constexpr int LEN = sizeof(kLabel) - 1;       // 3
    constexpr int GLYPH_W = 6;
    constexpr int GLYPH_H = 8;
    constexpr int X = (PANEL_WIDTH  - LEN * GLYPH_W) / 2;
    constexpr int Y = (PANEL_HEIGHT - GLYPH_H) / 2;

    matrix.fillScreen(0x0000);  // universal background
    matrix.setCursor(X, Y);
    matrix.print(kLabel);
    // matrix.show() is called by loop1() after chrome is layered on top
    // (phase 3.5.2 — FR-9.3).
  }
};

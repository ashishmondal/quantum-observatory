// Placeholder scene: cycles the panel through R/G/B/W with a black diagonal.
// Acts as the smoke-test scene until real scenes (boot, starfield, ...) land
// in Phases 2+. Carries no state beyond the parent class.
//
// (added in phase 1.3 as the reference Scene implementation)

#pragma once

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "scene.h"

class ColorCycleScene : public Scene {
public:
  const char* name() const override { return "color_cycle"; }

  void init(Adafruit_Protomatter& /*matrix*/) override {
    // Nothing to seed — palette is static.
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // Integer-only math (NFR-1.3). Cadence is one color per wall-clock second
    // so the visual is independent of frame rate.
    static const uint16_t COLORS[4] = {
        0xF800, // red
        0x07E0, // green
        0x001F, // blue
        0xFFFF, // white
    };
    const uint8_t idx = static_cast<uint8_t>((now_ms / 1000u) & 0x3u);

    matrix.fillScreen(COLORS[idx]);
    matrix.drawLine(0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1, 0x0000);
    // matrix.show() is called by loop1() after chrome (phase 3.5.2).
  }
};

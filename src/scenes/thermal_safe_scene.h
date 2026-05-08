// Thermal-safe scene — firmware-owned override (FR-7.3) shown when the
// DS3231 on-die temperature reads above the configured threshold. The
// dispatch into this scene is decided in scene_state::take_pending()
// based on thermal_monitor::is_hot(); HA cannot request it directly.
//
// Visual goal: minimum LED current (FR-7.5 spirit — actively shedding
// heat) while still telling a human "I'm hot, leave me alone for a
// bit". Single small dim red word "COOL" stacked over the current
// temperature (e.g. "53C"), centred on a black field. No chrome. No
// large bright glyphs. Updates piggy-back on thermal_monitor's slow
// poll; the scene re-renders every frame but reads a cached value.
//
// (added in phase 5.5.2)

#pragma once

#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "gfx_text.h"
#include "scene.h"
#include "theme.h"
#include "thermal_monitor.h"
// FreeSansBold9pt7b is what theme::font(HEADER) currently resolves to
// for Apollo (placeholder until T.6 swaps in Press Start 2P), so this
// scene picks up that font through the theme — no direct include needed.

class ThermalSafeScene : public Scene {
public:
  const char* name() const override { return "thermal_safe"; }

  // Same rationale as NightScene: the override IS the entire scene;
  // we don't want the global clock chrome stomping on the layout.
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    (void)now_ms;
    matrix.fillScreen(0x0000);

    matrix.setFont(theme::font(theme::FontRole::HEADER));
    matrix.setTextSize(1);

    // Deep red — theme::SAFETY ink, same as NightScene (low LED current).
    const uint16_t kInk  = theme::ink(theme::Ink::SAFETY);
    constexpr uint16_t kHalo = 0x0000;  // universal background

    // Top half: "COOL". Baseline ~13 puts the cap line near the top
    // of the panel given the FreeSansBold9pt7b ascent.
    const char* kTop = "COOL";
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, kTop), 13,
                        kTop, kInk, kHalo);

    // Bottom half: temperature, e.g. "53C". INT8_MIN sentinel from
    // thermal_monitor means "no read yet" — show "--C" instead so we
    // never print a garbage temperature on first paint.
    char tbuf[8];
    const int8_t t = thermal_monitor::last_temp_c();
    if (t == INT8_MIN) {
      tbuf[0]='-'; tbuf[1]='-'; tbuf[2]='C'; tbuf[3]='\0';
    } else {
      snprintf(tbuf, sizeof(tbuf), "%dC", static_cast<int>(t));
    }
    // Baseline 30 puts the bottom line near the bottom edge with one
    // pixel of breathing room.
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, tbuf), 30,
                        tbuf, kInk, kHalo);
    // matrix.show() is called by loop1().
  }
};

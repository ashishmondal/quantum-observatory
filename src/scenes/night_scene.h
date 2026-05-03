// Night scene — firmware-owned override (FR-7.2) shown when the on-board
// photoresistor reads below the configured threshold. The dispatch into
// this scene is decided in scene_state::take_pending() based on
// light_sensor::is_night(); HA cannot request it directly.
//
// Visual goal: minimum perceptible disturbance in a dark room. Single
// dim deep-red HH:MM, centered, on a black field. Red preserves dark
// adaptation (and looks calm); deep red keeps the LED current — and
// thus heat — minimal. No chrome (a second clock readout would be
// pointless and stomp on the centred time).
//
// Until the RTC has been read at least once the readout shows "--:--"
// per FR-9.6 — same masking convention as giant_clock_scene.
//
// (added in phase 5.5.1)

#pragma once

#include <stdio.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/FreeSansBold12pt7b.h>

#include "config.h"
#include "gfx_text.h"
#include "scene.h"
#include "time_of_day.h"

class NightScene : public Scene {
public:
  const char* name() const override { return "night"; }

  // FR-9.3 / "scene IS the clock" rationale (cf. giant_clock_scene).
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    matrix.fillScreen(0x0000);

    char hhmm[6];  // "HH:MM" + NUL
    const tod::Reading r = tod::now(now_ms);
    if (r.valid) {
      snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
               static_cast<int>(r.hour), static_cast<int>(r.minute));
    } else {
      hhmm[0]='-'; hhmm[1]='-'; hhmm[2]=':';
      hhmm[3]='-'; hhmm[4]='-'; hhmm[5]='\0';
    }

    matrix.setFont(&FreeSansBold12pt7b);
    matrix.setTextSize(1);
    // Deep red — RGB565 0x4000 ≈ R=8/31 — bright enough to read across
    // a small bedroom but ~25% of full red current. Cooler than the
    // giant clock's 0xFFFF white in every sense (FR-7.3 spirit).
    // No halo: the background is already pure black, and a red halo
    // would just smear the glyphs without adding legibility.
    constexpr uint16_t kInk  = 0x4000;
    constexpr uint16_t kHalo = 0x0000;
    // Same baseline as giant_clock_scene so the swap doesn't visibly
    // jump if the room flips into night mid-scene.
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, hhmm), 17,
                        hhmm, kInk, kHalo);
    // matrix.show() is called by loop1().
  }
};

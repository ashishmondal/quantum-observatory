// Sky timelapse scene — debug aid for the sun-position renderer.
//
// Sweeps a synthetic UTC epoch through one full local day every 10
// seconds, recomputing the sun position and gradient each frame. Lets
// you visually verify (a) the gradient bands transition cleanly, (b)
// the sun rises on the LEFT, peaks at noon over the centre, sets on
// the RIGHT, and (c) the night fallback engages between sets and
// rises.
//
// The math comes from the same SkyBg path used by giant_clock_scene
// — we just feed it a fake epoch instead of the RTC's. So if it
// looks wrong here, it'll look wrong on the real clock too.
//
// Layout: full-screen sky, "TIMELAPSE" label centred at the bottom
// in Picopixel cyan so you can tell at a glance it isn't the real
// clock.
//
// (added in phase 6.5+ polish, debug-only)

#pragma once

#include <stdio.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>

#include "backgrounds/sky_bg_render.h"
#include "config.h"
#include "gfx_text.h"
#include "scene.h"
#include "sun_position.h"

class SkyTimelapseScene : public Scene {
public:
  const char* name() const override { return "sky_timelapse"; }

  // Full-screen background — corner clock chrome would clash.
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // One synthetic day every kPeriodMs. Anchor at a known noon UTC
    // so the timelapse always starts mid-day for easy spotting.
    static constexpr uint32_t kPeriodMs = 10000u;
    static constexpr int32_t  kAnchorUtc = 1746360000;  // ~2025-05-04 12:00 UTC
    const uint32_t phase_ms = now_ms % kPeriodMs;
    const int32_t  fake_utc = kAnchorUtc
                            + static_cast<int32_t>((phase_ms * 86400u) / kPeriodMs);

    sky_bg_render::draw(matrix, fake_utc, LATITUDE_DEG, LONGITUDE_DEG);

    // Label.
    static const char kLabel[] = "TIMELAPSE";
    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, kLabel), /*y=*/31,
                        kLabel, /*ink=*/0x07FF, /*halo=*/0x0000);
  }
};

// Offline scene — firmware-owned override (FR-5.1) shown when MQTT
// is disconnected. The dispatch into this scene is decided in
// scene_state::take_pending() based on mqtt_link::connected(); HA
// cannot request it directly (the broker is unreachable when it
// matters anyway).
//
// Visual goal per §6 registry: "starfield_dim" + local time. The
// device is still the room clock during an outage (FR-9.1) so we keep
// the giant HH:MM readout on top of the existing starfield, and add a
// small "OFFLINE" badge in the bottom strip in place of the date so a
// glance reveals the broker is unreachable.
//
// Until the RTC has been read at least once the readout shows "--:--"
// per FR-9.6 — same masking convention as giant_clock_scene.
//
// (added in phase 6.4)

#pragma once

#include <stdio.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/Picopixel.h>

#include "backgrounds.h"
#include "config.h"
#include "gfx_text.h"
#include "scene.h"
#include "time_of_day.h"

class OfflineScene : public Scene {
public:
  const char* name() const override { return "offline"; }

  // FR-9.3 / "scene IS the clock" (cf. giant_clock_scene) — the giant
  // readout would clash with corner chrome.
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    g_backgrounds.render(BgType::STARFIELD, matrix, now_ms);

    // ── Time line (identical layout to giant_clock_scene so a swap
    // mid-frame doesn't visibly jump if MQTT drops while the giant
    // clock is active) ─────────────────────────────────────────────
    const tod::Reading r = tod::now(now_ms);
    char hhmm[6];
    if (r.valid) {
      snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
               static_cast<int>(r.hour), static_cast<int>(r.minute));
    } else {
      hhmm[0]='-'; hhmm[1]='-'; hhmm[2]=':';
      hhmm[3]='-'; hhmm[4]='-'; hhmm[5]='\0';
    }
    matrix.setFont(&FreeSansBold12pt7b);
    matrix.setTextSize(1);
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, hhmm), 17,
                        hhmm, 0xFFFF, 0x0000);

    // ── "OFFLINE" badge ─ replaces the date strip. Picopixel keeps
    // the strip the same height/baseline as giant_clock_scene's date
    // line (Y=30) so the swap is positionally clean. Amber-ish ink to
    // signal "warning, but not safety critical".
    static const char kBadge[] = "OFFLINE";
    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, kBadge), 30,
                        kBadge, 0xFD20 /* amber */, 0x0000);
  }
};

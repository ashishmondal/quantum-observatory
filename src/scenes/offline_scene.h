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
#include <string.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>

#include "backgrounds.h"
#include "config.h"
#include "fonts/digital_7__mono_14pt7b.h"
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
    // Same starfield bg as giant_clock_scene so the swap is visually
    // continuous when MQTT drops/recovers.
    g_backgrounds.render(BgType::IMAGE, matrix, now_ms);

    // ── Time line — identical layout to giant_clock_scene ───────────
    const tod::Reading r = tod::now(now_ms);
    char hhmm[6];
    if (r.valid) {
      uint8_t h12 = r.hour % 12;
      if (h12 == 0) h12 = 12;
      snprintf(hhmm, sizeof(hhmm), "%2u:%02u",
               static_cast<unsigned>(h12), static_cast<unsigned>(r.minute));
    } else {
      hhmm[0]='-'; hhmm[1]='-'; hhmm[2]=':';
      hhmm[3]='-'; hhmm[4]='-'; hhmm[5]='\0';
    }
    matrix.setFont(&digital_7__mono_14pt7b);
    matrix.setTextSize(1);
    gfx::draw_text_halo(matrix, /*x=*/2, /*y=*/17,
                        hhmm, 0xFFFF, 0x0000);

    // ── Divider (same row as giant_clock_scene) ─────────────────────
    matrix.drawFastHLine(0, 21, PANEL_WIDTH, 0x0010);

    // ── "OFFLINE" badge — replaces the date strip, amber to read as
    //    a soft warning rather than an error. Same baseline (Y=29) as
    //    giant_clock_scene's date so the swap is positionally clean.
    static const char kBadge[] = "OFFLINE";
    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, kBadge), /*y=*/29,
                        kBadge, 0xFD20 /* amber */, 0x0000);
  }
};

// Giant clock scene — the device's primary "room clock" view (FR-9.4).
//
// Layout (64×32):
//   rows ~3..18 : HH:MM in FreeSansBold12pt7b, centred. Largest font that
//                 fits the panel width with room for the date strip below.
//   rows ~22..27: "WED 01 MAY" in Picopixel, centred.
//   bg          : starfield (the "starfield_dim" call in §6 of REQUIREMENTS
//                 is satisfied by the existing starfield's already-low
//                 baseline brightness; a true dim variant is a future
//                 polish item, not a 3.5.3 blocker).
//
// Per FR-9.3 this scene opts OUT of the corner clock chrome (it would
// stomp on the giant readout). Until tod is initialised it shows
// "--:--" and "--- -- ---" per FR-9.6.
//
// (added in phase 3.5.3)

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

class GiantClockScene : public Scene {
public:
  const char* name() const override { return "clock"; }

  // FR-9.3: this scene IS the clock — suppress the chrome readout.
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    g_backgrounds.render(BgType::STARFIELD, matrix, now_ms);

    // ── Time line ────────────────────────────────────────────────────
    const tod::Reading r = tod::now(now_ms);
    char hhmm[6]; // "HH:MM" + NUL
    if (r.valid) {
      snprintf(hhmm, sizeof(hhmm), "%02d:%02d",
               static_cast<int>(r.hour), static_cast<int>(r.minute));
    } else {
      hhmm[0]='-'; hhmm[1]='-'; hhmm[2]=':'; hhmm[3]='-'; hhmm[4]='-'; hhmm[5]='\0';
    }
    matrix.setFont(&FreeSansBold12pt7b);
    matrix.setTextSize(1);
    // FreeSansBold12pt7b caps are ~16 px tall. Baseline Y = 17 puts caps
    // in rows ~2..17 with a 1-px halo above; clears the panel top.
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, hhmm), 17,
                        hhmm, 0xFFFF, 0x0000);

    // ── Date line ────────────────────────────────────────────────────
    // "WED 01 MAY" — 10 chars × ~4 px in Picopixel ≈ 40 px wide. Baseline
    // Y = 30 → glyphs span rows ~26..30, halo to 25..31. Within panel.
    char date[12]; // "WWW DD MMM" + NUL = 11
    if (r.valid) {
      int16_t  yr;
      uint8_t  mo, d, dow;
      tod::date_from_local_epoch(r.local_epoch, &yr, &mo, &d, &dow);
      snprintf(date, sizeof(date), "%s %02d %s",
               tod::weekday_abbrev(dow), static_cast<int>(d),
               tod::month_abbrev(mo));
    } else {
      // Length matches "WWW DD MMM" so centring is identical pre/post sync.
      const char* k = "--- -- ---";
      // 11 chars + NUL fits sizeof(date)=12.
      for (size_t i = 0; i <= 10; ++i) date[i] = k[i];
    }
    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, date), 30,
                        date, 0xFFFF, 0x0000);
  }
};

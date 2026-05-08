// Giant clock scene — the device's primary "room clock" view (FR-9.4).
//
// Layout (64×32) — modeled on a 7-segment LCD reference:
//
//   rows  0..17 : giant HH:MM (12-hour) in Digital-7 14pt, white
//                 5 chars × 12 px advance = 60 px wide, x=2..61
//   row    21   : thin horizontal divider, dim blue
//   rows 25..29 : date "SAT 18 MAY 2024" in Picopixel amber, centred
//   bg          : artist-supplied starfield.bmp via BG_IMAGE (FR-12.4)
//
// 12-hour display per user preference (FR-9.2 default is 24-hour but
// this scene overrides it to match the LCD-clock aesthetic). AM/PM and
// seconds are intentionally dropped — Digital-7 14pt is too chunky to
// fit a side column at 64 px wide. Until tod is initialised the scene
// shows "--:--" / placeholder date per FR-9.6.
//
// Per FR-9.3 this scene opts OUT of the corner clock chrome (it would
// stomp on the giant readout).
//
// (added in phase 3.5.3; redesigned in phase 6.5+ polish)

#pragma once

#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "backgrounds.h"
#include "config.h"
#include "gfx_text.h"
#include "scene.h"
#include "theme.h"
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
    // Background: live sky gradient + sun on an arc, computed from
    // the device's local time and the observer location in config.h.
    g_backgrounds.render(BgType::SKY, matrix, now_ms);

    const tod::Reading r = tod::now(now_ms);

    // ── Giant HH:MM (12-hour) ────────────────────────────────────────
    // Convert 24h → 12h for display only; scene-internal, doesn't
    // touch the canonical tod::now() value.
    char hhmm[6];  // "HH:MM" + NUL
    if (r.valid) {
      uint8_t h12 = r.hour % 12;
      if (h12 == 0) h12 = 12;
      snprintf(hhmm, sizeof(hhmm), "%2u:%02u",
               static_cast<unsigned>(h12), static_cast<unsigned>(r.minute));
    } else {
      hhmm[0]='-'; hhmm[1]='-'; hhmm[2]=':';
      hhmm[3]='-'; hhmm[4]='-'; hhmm[5]='\0';
    }
    // Digital-7 14pt: glyph 18 px tall, yOffset -17, advance 12 px.
    // Baseline y=19 puts glyph top at y+yOffset = 2 → fits rows 2..19.
    // 12-hour format means the leading char is always blank or '1', so
    // the active readout is 4 chars × 12 px = 48 px wide. Anchor flush
    // left at x=2; the leading-digit slot only ever shows '1' (for
    // 10/11/12) and is blank otherwise.
    matrix.setFont(theme::font(theme::FontRole::CLOCK));
    matrix.setTextSize(1);

    // ── LCD ghost layer (with halo) ──────────────────────────────────
    // Real 7-segment LCDs show every unlit segment as a faint shadow.
    // We mimic that by drawing "18:88" (the union of all segments that
    // can ever light in 12-hour mode) in dim grey *with* a black halo
    // first — the halo punches a clean hole in the starfield so the
    // ghost reads as etched. Then the live time overlays in plain
    // white, no halo (the ghost+halo already provides the contrast
    // edge). Net cost: one halo pass instead of two.
    gfx::draw_text_halo(matrix, /*x=*/2, /*y=*/19,
                        "18:88",
                        theme::ink(theme::Ink::GHOST),
                        /*halo=*/0x0000);  // universal background

    // ── Live digits, no halo ────────────────────────────────────────
    matrix.setTextColor(theme::ink(theme::Ink::GIANT_DIGITS));
    matrix.setCursor(2, 19);
    matrix.print(hhmm);

    // ── Divider ─────────────────────────────────────────────────────
    // Dim warm green under Apollo — same low-luminance "glow" feel as
    // the amber date strip below, but in a complementary hue so the
    // divider reads as a separate UI element rather than an extension
    // of the date.
    matrix.drawFastHLine(0, 22, PANEL_WIDTH,
                         theme::ink(theme::Ink::DIVIDER));

    // ── Date strip ──────────────────────────────────────────────────
    // Apollo BODY ink is deep amber (0xF940) — RGB(255,80,0). Dropping
    // green pulls the hue away from yellow toward burnt orange so it
    // doesn't visually merge with white digits above. Other themes
    // override BODY to their own data ink.
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);
    char date[16];  // "SAT 18 MAY 2024" + NUL = 16
    if (r.valid) {
      int16_t  yr;
      uint8_t  mo, d, dow;
      tod::date_from_local_epoch(r.local_epoch, &yr, &mo, &d, &dow);
      snprintf(date, sizeof(date), "%s %02u %s %04d",
               tod::weekday_abbrev(dow), static_cast<unsigned>(d),
               tod::month_abbrev(mo), static_cast<int>(yr));
    } else {
      strncpy(date, "--- -- --- ----", sizeof(date));
      date[sizeof(date)-1] = '\0';
    }
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, date), /*y=*/29,
                        date,
                        theme::ink(theme::Ink::BODY),
                        theme::ink(theme::Ink::BODY_HALO));
  }
};

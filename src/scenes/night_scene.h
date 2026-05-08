// Night scene — firmware-owned override (FR-7.2) shown when the on-board
// photoresistor reads below the configured threshold. The dispatch into
// this scene is decided in scene_state::take_pending() based on
// light_sensor::is_night(); HA cannot request it directly.
//
// Visual goal: minimum perceptible disturbance in a dark room while
// still showing the device's primary "room clock" view. We render the
// same layout as giant_clock_scene (HH:MM in the active theme's CLOCK
// font + divider + date strip in BODY font) but force every ink to a
// uniform deep red (0x4000, ~25% red current) and skip ALL animated
// background / theme banner painting. Rationale:
//   - Red preserves dark adaptation; deep red keeps LED current low.
//   - Reusing the giant-clock layout means the visual jump when the
//     room flips into/out of night mode is just a color+motion change,
//     not a re-layout.
//   - 0x4000 is hardcoded (not theme::Ink::SAFETY) because LCARS
//     defines SAFETY as full red 0xF800 for red-alert use — far too
//     bright for a bedroom at 3 AM.
//
// Until the RTC has been read at least once the readout shows "--:--"
// per FR-9.6 — same masking convention as giant_clock_scene.
//
// (added in phase 5.5.1; redesigned to mirror giant_clock layout)

#pragma once

#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "gfx_text.h"
#include "scene.h"
#include "theme.h"
#include "time_of_day.h"

class NightScene : public Scene {
public:
  const char* name() const override { return "night"; }

  // FR-9.3 / "scene IS the clock" rationale (cf. giant_clock_scene).
  bool wants_clock_chrome() const override { return false; }

  // Suppress the theme decorations umbrella pass — Blade Runner /
  // LCARS would otherwise paint a bright cyan/orange FRAME_BORDER
  // over our deep-red field, defeating the whole point of night mode.
  bool wants_theme_decorations() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

private:
  // Mirror of giant_clock_scene::date_x_nudge — Org_01 (BLADE_RUNNER /
  // LCARS_TOS BODY font) is optically off-centre by 2 px left of its
  // geometric centre. Other themes use Picopixel/TomThumb which are
  // already centred. Kept in sync by hand: this is a 2-line table.
  static int16_t date_x_nudge() {
    switch (theme::current()) {
      case theme::Id::BLADE_RUNNER:
      case theme::Id::LCARS_TOS:
        return 2;
      default:
        return 0;
    }
  }

public:

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // Solid black field — no animated bg in night mode (FR-7.2 spirit:
    // minimum perceptible disturbance, zero motion).
    matrix.fillScreen(0x0000);

    // Uniform deep red for every glyph. See header comment for why
    // we don't use theme::Ink::SAFETY.
    constexpr uint16_t kInk = 0x4000;

    const tod::Reading r = tod::now(now_ms);

    // ── Giant HH:MM (12-hour, matches giant_clock_scene) ────────────
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
    // Same font / baseline / x-anchor as giant_clock_scene so the swap
    // doesn't visibly shift the digits. No ghost "18:88" layer (it
    // would add visual noise on a black field where the contrast edge
    // is already free) and no halo (deep red on pure black is clean).
    matrix.setFont(theme::font(theme::FontRole::CLOCK));
    matrix.setTextSize(1);
    matrix.setTextColor(kInk);
    matrix.setCursor(2, 19);
    matrix.print(hhmm);

    // ── Divider — drawn unconditionally (no animated bg to clash
    //    with under any theme).
    matrix.drawFastHLine(0, 22, PANEL_WIDTH, kInk);

    // ── Date strip ──────────────────────────────────────────────────
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);
    char date[16];  // "SAT 18 MAY 2024" + NUL
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
    matrix.setTextColor(kInk);
    // Per-font baseline correction (THEME.md §2.3): TomThumb sits one
    // row above other BODY faces.
    const int16_t date_y = 29 + theme::baseline_y_shift(theme::FontRole::BODY);
    matrix.setCursor(gfx::centered_x(matrix, date) + date_x_nudge(), date_y);
    matrix.print(date);
    // matrix.show() is called by loop1().
  }
};

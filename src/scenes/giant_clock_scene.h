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
#include "scenes/bayer_dither.h"
#include "theme.h"
#include "time_of_day.h"

class GiantClockScene : public Scene {
public:
  const char* name() const override { return "clock"; }

  // FR-9.3: this scene IS the clock — suppress the chrome readout.
  bool wants_clock_chrome() const override { return false; }

private:
  // Per-theme horizontal nudge for the centred date strip. Blade Runner
  // and LCARS both render their date in Org_01 (5×6 sans w/ true
  // lowercase); under those themes the optical centre of the glyph
  // strip sits ~2 px left of the geometric centre returned by
  // gfx::centered_x() — pushing the strip right by 2 px restores the
  // visual centring. Apollo/Nostromo/Vectrex use Picopixel/TomThumb
  // which are already optically centred, so they get 0.
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

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // Background: per-theme animated ambience (FR-15) — see
    // src/backgrounds/theme_clock_bg.h. Each theme gets its own
    // signature motion behind the LCD readout; the live digits self-
    // clean via the halo passes below so any bg pattern is safe.
    g_backgrounds.render(BgType::THEME_CLOCK, matrix, now_ms);

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
    // Suppressed under themes whose animated background already provides
    // strong horizontal motion behind the giant digits — Apollo (CRT
    // raster scan) and Vectrex (perspective grid lines). A static
    // divider on top of those just reads as visual clutter.
    const theme::Id tid = theme::current();
    if (tid != theme::Id::APOLLO_AMBER &&
        tid != theme::Id::VECTREX_NEON) {
      matrix.drawFastHLine(0, 22, PANEL_WIDTH,
                           theme::ink(theme::Ink::DIVIDER));
    }

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
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, date) + date_x_nudge(), /*y=*/29,
                        date,
                        theme::ink(theme::Ink::BODY),
                        theme::ink(theme::Ink::BODY_HALO));

    // ── Theme-change banner ─────────────────────────────────────────
    // After a theme switch we want the new theme's name to "announce
    // itself" on a black field, then dissolve away to reveal the
    // freshly-themed clock — the same Bayer-dither dissolve the
    // compositor uses between scenes (see scenes/fade_black_layer.h).
    //
    // Phases (millis since the switch):
    //   0   .. HOLD              alpha = 255 → full black, draw the
    //                            two-line theme name on top.
    //   HOLD .. HOLD + DISSOLVE  alpha = 255 → 0 → bayer dissolve
    //                            reveals the bg+clock underneath; the
    //                            text dissolves with it (we stop
    //                            painting it, so the dither carves
    //                            holes through the black covering it).
    //   else                     no overlay, no banner.
    //
    // Boots silent: theme::last_change_ms() returns 0 until the first
    // switch (compared via wrap-safe subtraction so the millis()
    // counter rolling past 0 around day 49 doesn't fire it).
    constexpr uint32_t kHoldMs     = 1200;
    constexpr uint32_t kDissolveMs = 500;
    constexpr uint32_t kBannerMs   = kHoldMs + kDissolveMs;
    const uint32_t since = now_ms - theme::last_change_ms();
    if (theme::last_change_ms() != 0 && since < kBannerMs) {
      uint16_t alpha;
      bool     draw_text;
      if (since < kHoldMs) {
        alpha     = 255;
        draw_text = true;
      } else {
        const uint32_t t = since - kHoldMs;
        alpha     = static_cast<uint16_t>(255u - (t * 255u) / kDissolveMs);
        draw_text = false;
      }

      // Black overlay first — punches holes (or the whole screen at
      // alpha=255) through the bg+clock that already drew above.
      bayer::apply_black_overlay(matrix, alpha);

      if (draw_text) {
        // Split the display name at its single space so the label
        // renders as two centred lines. display_name() is owned by
        // theme.cpp and contains exactly one space (e.g.
        // "APOLLO AMBER", "NOSTROMO GREEN").
        const char* full = theme::display_name(theme::current());
        char line1[16];
        const char* sp = strchr(full, ' ');
        const char* line2;
        if (sp != nullptr) {
          const size_t n = static_cast<size_t>(sp - full);
          const size_t copy = (n < sizeof(line1) - 1) ? n : sizeof(line1) - 1;
          memcpy(line1, full, copy);
          line1[copy] = '\0';
          line2 = sp + 1;
        } else {
          // Single-word fallback — render the whole thing on line 1.
          strncpy(line1, full, sizeof(line1) - 1);
          line1[sizeof(line1) - 1] = '\0';
          line2 = "";
        }

        matrix.setFont(theme::font(theme::FontRole::HEADER));
        matrix.setTextSize(1);
        // Two stacked lines, baselines at y=14 and y=26 — places the
        // two glyph blocks roughly centred on the panel for the
        // current HEADER fonts (ascent ~7..8 px each).
        const int16_t cx1 = gfx::centered_x(matrix, line1);
        gfx::draw_text_halo(matrix, cx1, /*y=*/14, line1,
                            theme::ink(theme::Ink::HEADER),
                            theme::ink(theme::Ink::HEADER_HALO));
        if (line2[0] != '\0') {
          const int16_t cx2 = gfx::centered_x(matrix, line2);
          gfx::draw_text_halo(matrix, cx2, /*y=*/26, line2,
                              theme::ink(theme::Ink::HEADER),
                              theme::ink(theme::Ink::HEADER_HALO));
        }
      }
    }
  }
};

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
#include "clock_anim_test.h"
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
  // ── Giant HH:MM slot geometry ───────────────────────────────────
  // Digital-7 14pt: glyph 10×18, advance 12 px, baseline y=19 →
  // glyph occupies rows 2..19. Five character cells flush-left at
  // x=2 (H1 H2 ':' M1 M2). Cascade animation works per-slot.
  static constexpr int16_t kSlotX[5]  = { 2, 14, 26, 38, 50 };
  static constexpr int16_t kSlotW     = 12;
  static constexpr int16_t kSlotTop   = 2;
  static constexpr int16_t kSlotH     = 18;
  static constexpr int16_t kBaselineY = 19;

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

  // ── Digit-roll animation state ──────────────────────────────────
  // m_prev_hhmm holds the latched (post-cascade) string; idle slots
  // paint from this. m_anim_start[i]==0 means idle; otherwise it's
  // the millis() at which the slot's roll began. m_anim_dur_ms[i]
  // is the slot's total animation duration (300 / 600 / 900). Slot 0
  // (H1) and slot 2 (':') are never animated.
  char     m_prev_hhmm[6]   = "--:--";
  uint32_t m_anim_start[5]  = {0, 0, 0, 0, 0};
  uint16_t m_anim_dur_ms[5] = {0, 0, 0, 0, 0};
  // Last step index emitted to the buzzer click counter, per slot.
  // 0xFFFF = "no step yet"; reset on cascade start. Used to gate
  // the cross-core click sequence increment so each visible digit
  // tick produces exactly one audible click.
  uint16_t m_anim_last_step[5] = {0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
  // Per-slot RNG mix is `(m_seed ^ slot * 0x9E3779B9u ^ step_idx)`.
  // m_seed never changes — the (slot, step) salt gives stable
  // per-step digits across repeated frames within a step, while
  // varying across slots and across the cascading frames within a
  // single tick. Same constant the BR/Nostromo themes use.
  uint32_t m_seed = 0xA5F03C2Du;

  // Linearly scale every channel of an RGB565 ink toward 0 by an
  // 8-bit alpha. Used by the colon fade-out so the chosen theme's
  // GIANT_DIGITS hue stays intact across the brightness ramp.
  static uint16_t scale_rgb565(uint16_t c, uint16_t a_q8) {
    const uint16_t r5 = (c >> 11) & 0x1Fu;
    const uint16_t g6 = (c >>  5) & 0x3Fu;
    const uint16_t b5 =  c        & 0x1Fu;
    const uint16_t r  = static_cast<uint16_t>((r5 * a_q8) >> 8);
    const uint16_t g  = static_cast<uint16_t>((g6 * a_q8) >> 8);
    const uint16_t b  = static_cast<uint16_t>((b5 * a_q8) >> 8);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
  }

  // Repaint the slot's char from "18:88" in Ink::GHOST so the LCD
  // ghost layer survives the slot's black-fill at the start of every
  // animation frame. Caller has already set the CLOCK font.
  void redraw_ghost_for_slot(Adafruit_Protomatter& matrix, int slot_idx) {
    static constexpr char kGhost[6] = "18:88";
    matrix.setTextColor(theme::ink(theme::Ink::GHOST));
    matrix.setCursor(kSlotX[slot_idx], kBaselineY);
    matrix.write(static_cast<uint8_t>(kGhost[slot_idx]));
  }

  // Two-phase cascading roll: a constant-speed 30 ms/step scramble
  // for `(dur_ms - 300) ms`, followed by a fixed 300 ms ease-out
  // cubic settle on `settle_ch`. The trigger slot has const phase = 0
  // (pure ease); each cascade step down adds 300 ms of constant-speed
  // prelude so the lower-order slots keep ticking at a uniform digit
  // cadence while the upper slot is still easing.
  void render_digit_roll(Adafruit_Protomatter& matrix, int slot_idx,
                         char settle_ch, uint32_t elapsed_ms,
                         uint16_t dur_ms) {
    // Wipe the slot back to the panel's universal background and
    // restore the dim LCD ghost glyph behind the active digit.
    matrix.fillRect(kSlotX[slot_idx], kSlotTop, kSlotW, kSlotH, 0x0000);
    redraw_ghost_for_slot(matrix, slot_idx);

    constexpr uint16_t kEaseMs    = 1000;
    constexpr uint16_t kStepMs    = 30;
    constexpr int      kEaseSteps = 33;

    const uint16_t const_ms =
        (dur_ms > kEaseMs) ? static_cast<uint16_t>(dur_ms - kEaseMs) : 0u;

    char ch;
    uint16_t cur_step;
    if (elapsed_ms < const_ms) {
      // Constant-speed phase — uniform 30 ms/step digit cycling.
      const uint32_t step_idx = elapsed_ms / kStepMs;
      cur_step = static_cast<uint16_t>(step_idx);
      const uint32_t mix = m_seed
                           ^ (static_cast<uint32_t>(slot_idx) * 0x9E3779B9u)
                           ^ step_idx;
      ch = static_cast<char>('0' + (mix % 10u));
    } else {
      // Ease-out cubic phase, scaled to integer Q10. Maps the
      // remaining 0..300 ms onto kEaseSteps so the digit visibly
      // decelerates into the settle char.
      const uint32_t te = elapsed_ms - const_ms;
      uint32_t t_q10 = (te * 1024u) / kEaseMs;
      if (t_q10 > 1024u) t_q10 = 1024u;
      const uint32_t inv   = 1024u - t_q10;
      const uint32_t e_q10 = 1024u - ((inv * inv / 1024u) * inv / 1024u);
      int ease_step = static_cast<int>((e_q10 * kEaseSteps) / 1024u);
      // Encode ease-phase step into the cross-phase index space so
      // edge-detect against m_anim_last_step never collides with a
      // const-phase value (which uses raw elapsed/30 and is small).
      cur_step = static_cast<uint16_t>(2000 + ease_step);
      if (ease_step >= kEaseSteps - 1) {
        ch = settle_ch;
      } else {
        // Offset the RNG salt by 1000 so ease-phase steps don't
        // collide with the constant-phase sequence (avoids "same
        // digit twice in a row" at the phase boundary).
        const uint32_t mix =
            m_seed
            ^ (static_cast<uint32_t>(slot_idx) * 0x9E3779B9u)
            ^ static_cast<uint32_t>(1000 + ease_step);
        uint32_t d = mix % 10u;
        // On the penultimate ease step, avoid landing on the settle
        // char so the final click reads as a real change instead of
        // a held frame.
        if (ease_step == kEaseSteps - 2 &&
            static_cast<char>('0' + d) == settle_ch) {
          d = (d + 1u) % 10u;
        }
        ch = static_cast<char>('0' + d);
      }
    }
    // Edge-detect step changes — each new step rings one click on
    // Core 0. Cheap when stepping (one volatile write); silent when
    // the digit is held (most frames during the slow ease tail).
    // Pack slot index into the low 3 bits so Core 0 can pick the
    // per-slot pitch from a single atomic read (see
    // clock_anim_test.h for the packing contract).
    if (cur_step != m_anim_last_step[slot_idx]) {
      m_anim_last_step[slot_idx] = cur_step;
      const uint32_t prev   = g_clock_anim_click_seq;
      const uint32_t prev_n = prev >> 3;
      g_clock_anim_click_seq =
          ((prev_n + 1u) << 3) | (static_cast<uint32_t>(slot_idx) & 0x7u);
    }
    matrix.setTextColor(theme::ink(theme::Ink::GIANT_DIGITS));
    matrix.setCursor(kSlotX[slot_idx], kBaselineY);
    matrix.write(static_cast<uint8_t>(ch));
  }

  // 1 Hz colon pulse: full brightness for the first 100 ms of each
  // wall-clock second, then a linear fade-out to a ~10% floor over
  // the remaining 900 ms. Floor is non-zero so the colon stays
  // legible as a separator throughout the second.
  void render_colon(Adafruit_Protomatter& matrix, uint32_t now_ms, bool valid) {
    // No fillRect / no halo behind the colon: the slot stays
    // transparent so the per-theme animated background (FR-15) shows
    // through between the two colon dots. The ghost ':' + faded live
    // ':' below paint only the glyph pixels themselves.
    matrix.setFont(theme::font(theme::FontRole::CLOCK));
    matrix.setTextSize(1);

    if (!valid) {
      // FR-9.6: no guessed time before first sync.
      matrix.setTextColor(theme::ink(theme::Ink::GIANT_DIGITS));
      matrix.setCursor(kSlotX[2], kBaselineY);
      matrix.write(static_cast<uint8_t>('-'));
      return;
    }

    // Restore the dim ghost ':' under the live colon — same role as
    // the per-slot ghost in render_digit_roll.
    matrix.setTextColor(theme::ink(theme::Ink::GHOST));
    matrix.setCursor(kSlotX[2], kBaselineY);
    matrix.write(static_cast<uint8_t>(':'));

    const uint32_t phase_ms = now_ms % 1000u;
    uint16_t alpha_q8;
    if (phase_ms < 100u) {
      alpha_q8 = 255;
    } else {
      constexpr uint16_t kFloor = 26;  // ~10% of 255
      alpha_q8 = static_cast<uint16_t>(
          255u - ((phase_ms - 100u) * (255u - kFloor)) / 900u);
    }
    const uint16_t faded =
        scale_rgb565(theme::ink(theme::Ink::GIANT_DIGITS), alpha_q8);
    matrix.setTextColor(faded);
    matrix.setCursor(kSlotX[2], kBaselineY);
    matrix.write(static_cast<uint8_t>(':'));
  }

  // Start a cascade rooted at `trigger_slot`. Walks the animatable
  // ladder (H2 → M1 → M2) from `trigger_slot` rightward and seeds
  // each slot with a 300/600/900 ms duration. Settle char is the
  // current live digit, so downstream slots animate even when their
  // value didn't change — the rolodex/odometer cascade feel.
  void start_cascade_from(int trigger_slot, const char* hhmm,
                          uint32_t now_ms) {
    static constexpr int kAnim[3] = { 1, 3, 4 };  // H2, M1, M2
    int trig_pos = 0;
    for (int i = 0; i < 3; ++i) {
      if (kAnim[i] == trigger_slot) { trig_pos = i; break; }
    }
    // Avoid the m_anim_start==0 "idle" sentinel — millis() is 0 only
    // briefly at boot but the test trigger could fire arbitrarily
    // early; cheap to guard.
    const uint32_t start_ms = (now_ms == 0u) ? 1u : now_ms;
    for (int i = trig_pos; i < 3; ++i) {
      const int slot = kAnim[i];
      m_anim_start[slot]  = start_ms;
      m_anim_dur_ms[slot] =
          static_cast<uint16_t>(1000u * (i - trig_pos + 1));
      m_prev_hhmm[slot]   = hhmm[slot];
      m_anim_last_step[slot] = 0xFFFFu;  // re-arm click edge-detect
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
    //
    // Colon slot intentionally blanked to a space here so the halo
    // pass leaves no black pixels behind the ':' — render_colon()
    // paints the ghost+live colon glyph alone, letting the animated
    // background show through the gaps between the two dots.
    gfx::draw_text_halo(matrix, /*x=*/2, /*y=*/19,
                        "18 88",
                        theme::ink(theme::Ink::GHOST),
                        /*halo=*/0x0000);  // universal background

    // ── Dev test trigger ─────────────────────────────────────────────
    // Edge-detect a cross-core synthetic-cascade request fired from
    // IR (LEFT/RIGHT/OK on the CLOCK scene under -DCLOCK_ANIM_TEST)
    // or MQTT (`observatory/test/clock_anim`). Sentinel 0 = no event;
    // values 1/2/3 map to MINUTE/TEN_MIN/HOUR cascades.
    {
      const uint8_t kind = g_clock_anim_test_kind;
      if (kind != 0u) {
        g_clock_anim_test_kind = 0u;
        int trigger_slot = -1;
        switch (static_cast<clock_anim_test::Kind>(kind)) {
          case clock_anim_test::Kind::MINUTE:  trigger_slot = 4; break;
          case clock_anim_test::Kind::TEN_MIN: trigger_slot = 3; break;
          case clock_anim_test::Kind::HOUR:    trigger_slot = 1; break;
          default: break;
        }
        if (trigger_slot >= 0) {
          start_cascade_from(trigger_slot, hhmm, now_ms);
        }
      }
    }

    // ── Per-slot live digits with cascading roll animation ──────────
    // Slots 0..4 = H1 H2 ':' M1 M2. H1 (slot 0) and ':' (slot 2)
    // never roll — H1 is blank or '1' only, the colon pulses on its
    // own 1 Hz schedule. M2/M1/H2 detect changes and start a
    // cascading roll keyed off the highest-order changed slot.
    const bool ch_h2 = (hhmm[1] != m_prev_hhmm[1]);
    const bool ch_m1 = (hhmm[3] != m_prev_hhmm[3]);
    const bool ch_m2 = (hhmm[4] != m_prev_hhmm[4]);

    // Silent-latch: any change touching '-' (boot or RTC dropout)
    // updates m_prev_hhmm without triggering a cascade. Suppresses
    // the boot-time five-slot stampede and the dropout's reverse.
    const bool involves_dash =
        (ch_h2 && (hhmm[1] == '-' || m_prev_hhmm[1] == '-')) ||
        (ch_m1 && (hhmm[3] == '-' || m_prev_hhmm[3] == '-')) ||
        (ch_m2 && (hhmm[4] == '-' || m_prev_hhmm[4] == '-'));

    if (involves_dash) {
      memcpy(m_prev_hhmm, hhmm, sizeof(m_prev_hhmm));
    } else if (ch_h2 || ch_m1 || ch_m2) {
      // Highest-order changed slot wins; cascade fills lower-order.
      const int trigger_slot = ch_h2 ? 1 : (ch_m1 ? 3 : 4);
      start_cascade_from(trigger_slot, hhmm, now_ms);
    }

    // Always keep the colon position in m_prev_hhmm tracking the
    // live string so a valid→invalid→valid cycle latches cleanly
    // on the colon (involves_dash already covers the digit slots).
    m_prev_hhmm[0] = hhmm[0];
    m_prev_hhmm[2] = hhmm[2];

    // H1 (idx 0): direct paint of live char. Skip on blank so the
    // ghost "1" stays visible underneath (matches the original
    // print(hhmm) where ' ' was a transparent glyph).
    if (hhmm[0] != ' ') {
      matrix.setTextColor(theme::ink(theme::Ink::GIANT_DIGITS));
      matrix.setCursor(kSlotX[0], kBaselineY);
      matrix.write(static_cast<uint8_t>(hhmm[0]));
    }

    // Animatable slots: H2 (1), M1 (3), M2 (4).
    static constexpr int kAnimSlotsForRender[3] = { 1, 3, 4 };
    for (int slot : kAnimSlotsForRender) {
      if (m_anim_start[slot] == 0u) {
        // Idle — paint the latched char in GIANT_DIGITS over the
        // already-drawn ghost layer.
        matrix.setTextColor(theme::ink(theme::Ink::GIANT_DIGITS));
        matrix.setCursor(kSlotX[slot], kBaselineY);
        matrix.write(static_cast<uint8_t>(m_prev_hhmm[slot]));
        continue;
      }
      const uint32_t elapsed = now_ms - m_anim_start[slot];
      const uint16_t dur     = m_anim_dur_ms[slot];
      if (elapsed >= dur) {
        // Animation complete — return to idle, paint settle char.
        m_anim_start[slot] = 0u;
        matrix.fillRect(kSlotX[slot], kSlotTop, kSlotW, kSlotH, 0x0000);
        redraw_ghost_for_slot(matrix, slot);
        matrix.setTextColor(theme::ink(theme::Ink::GIANT_DIGITS));
        matrix.setCursor(kSlotX[slot], kBaselineY);
        matrix.write(static_cast<uint8_t>(m_prev_hhmm[slot]));
      } else {
        render_digit_roll(matrix, slot, m_prev_hhmm[slot], elapsed, dur);
      }
    }

    // Colon (idx 2): 1 Hz pulse with linear fade-out to ~10% floor.
    render_colon(matrix, now_ms, r.valid);

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
    gfx::draw_text_halo(matrix, gfx::centered_x(matrix, date) + date_x_nudge(),
                        /*y=*/29,
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

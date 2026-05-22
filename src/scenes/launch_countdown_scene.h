// launch_countdown scene — next-scheduled-rocket-launch T-minus
// readout (FR-14.6, phase L).
//
// Layout (64×32, corner clock chrome enabled on top-right):
//   y= 0..7   "[LNCH]" identity header on the left (theme ALERT ink,
//             1.5 s dim pulse). Corner clock chrome shares the row
//             on the right; it's painted by the compositor on top
//             of the scene so the header just stays at x = 1.
//   y=10..17  "T-HH:MM:SS" in the built-in 6×8 mono font. Each of
//             the six H/M/S digits is its own odometer cell — when
//             the digit value changes, the new glyph slides down
//             from above and pushes the old glyph out the bottom
//             over kAnimMs ms. Above- and below-cell bleed is
//             clipped with two fillRect mask bands every frame.
//   y=18..23  Typewriter info row, TomThumb 3×5 baseline at y=22 so
//             the body occupies y=18..22 and the 1-px descender
//             row sits at y=23. Cycles one slide at a time (MISSION
//             / VEH / PAD / LIFTOFF / NET-or-CONFIRMED / WIN / and
//             after t0 a RESULT slide) with a typewriter reveal +
//             hold + blank-gap pattern. Same STATUS_DIM ink as the
//             marquee — secondary to the odometer. Blank when the
//             snapshot is stale (no point repeating the marquee's
//             AWAITING SCHEDULE on a second row).
//   y=25..31  Bottom marquee, TomThumb 3×5 font baseline-aligned at
//             y=30 so descenders of g/p/q/y remain visible on row
//             31 instead of being clipped. Scrolls the
//             mission.description prose at ~17 px/s in STATUS_DIM
//             ink (recessive so the countdown stays the focus).
//             TomThumb is hardcoded here regardless of theme so the
//             prose stays visually subordinate to the odometer —
//             a heavier theme font would compete with it for the
//             reader's eye. When the snapshot is stale the marquee
//             swaps to "AWAITING SCHEDULE" so the operator sees the
//             freshness gate explicitly; when fresh but description
//             is empty it falls back to the PROV/VEHICLE/MISSION/PAD
//             tag line.
//
// Render rules on integer t_minus seconds (NFR-1.3 — no float in
// render loop):
//   0 ≤ t_minus < kMaxCountdownS   -> "T-HH:MM:SS" (odometer down).
//                                     The final 60 s pulses the
//                                     digit ink 2 Hz between ALERT
//                                     and STATUS_WARN for urgency.
//   -kMaxPostT0S < t_minus < 0     -> "T+HH:MM:SS" (odometer up).
//                                     Steady STATUS_INFO ink — the
//                                     countdown urgency is over,
//                                     the row is now reporting time
//                                     since liftoff. The typewriter
//                                     row's RESULT slide tells the
//                                     operator how the launch
//                                     resolved (IN FLIGHT / SUCCESS
//                                     / FAIL / PARTIAL) per the
//                                     publisher's post-t0 hold rule.
//   no fresh snapshot, no RTC,
//   or t_minus outside the union
//   of the above two windows       -> "T- --:--:--" frozen. Odometer
//                                    state is reset to '-' so the
//                                    first valid frame snaps to the
//                                    live value without animating
//                                    from a dash → digit.
//
// Audio (L.6, FR-14.6, gated by FR-19.4 button_sound + FR-10.8 /
// FR-10.9 quiet): one short tick on the rising edge of each integer
// second in the [0, 10s] T-minus window, then one rising "ignition"
// sting at the falling edge through T-0. Unchanged by the visual
// redesign — audio still derives from raw t_minus regardless of
// whether the digit cells are showing data or dashes.

#pragma once

#include <cstring>
#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "buzzer.h"
#include "config.h"
#include "fonts/tomthumb_shifted.h"
#include "gfx_text.h"
#include "launch_state.h"
#include "prefs.h"
#include "scene.h"
#include "theme.h"
#include "time_of_day.h"

class LaunchCountdownScene : public Scene {
public:
  const char* name() const override { return "launch_countdown"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    // Reset per-second tick edge state so a re-init doesn't re-fire
    // ticks that already played on the previous activation.
    m_last_tick_sec    = -1;
    m_ignition_fired   = false;
    // Reset odometer cells. '-' is the sentinel for "no prior
    // value" — the first frame with valid data will snap each cell
    // to its digit without animating, so we don't see a one-shot
    // dash→digit slide on the very first valid readout.
    for (uint8_t i = 0; i < 6; ++i) {
      m_digits[i].cur           = '-';
      m_digits[i].prev          = '-';
      m_digits[i].anim_start_ms = 0;
    }
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // ── Background ────────────────────────────────────────────────
    // Solid black canvas. The minimalist layout is the whole point
    // — nothing animates outside the digit row, so we don't need a
    // textured background to fill empty pixels.
    matrix.fillScreen(0x0000);

    // ── Snapshot + integer T-minus arithmetic (NFR-1.3, no float) ─
    launch_state::Snapshot ls;
    const bool fresh = launch_state::get(now_ms, &ls);

    // tod::now() returns HA-local-epoch — the same frame the
    // launch payload was published in (HA's pyscript converts the
    // upstream UTC once at the producer) so the countdown is a pure
    // subtraction, with zero tz state on the firmware.
    int32_t  now_local  = 0;
    bool     have_clock = false;
    const tod::Reading r = tod::now(now_ms);
    if (r.valid) {
      now_local  = r.local_epoch;
      have_clock = true;
    }

    int32_t t_minus = 0;
    const bool have_t = fresh && have_clock;
    if (have_t) {
      t_minus = ls.t0_local_epoch - now_local;
    }
    // Digit cells render real data in two windows:
    //   * 0 ≤ t_minus < kMaxCountdownS   → T-HH:MM:SS countdown
    //   * -kMaxPostT0S < t_minus < 0     → T+HH:MM:SS count-up
    // Outside either window (no snapshot, no RTC, far-past, or
    // > 99:59:59 future) we freeze on dashes so a stale or out-of-
    // range snapshot can't drive a misleading readout.
    const bool data_valid =
        have_t &&
        ((t_minus >= 0 && t_minus < kMaxCountdownS) ||
         (t_minus < 0  && t_minus > -kMaxPostT0S));
    // True only when we're in the count-up half of `data_valid`.
    // Drives the T+ vs T- prefix swap, suppresses the final-minute
    // pulse, and steers the typewriter row to its RESULT slide.
    const bool post_t0 = data_valid && (t_minus < 0);

    // ── Audio cues (L.6) ─────────────────────────────────────────
    // Per-second tick in the [0, 10] window + one ignition sting on
    // the rising edge through T-0. Both gated on `button_sound`
    // (FR-19.4) AND the driver-side FR-10.8 / FR-10.9 quiet which
    // composes for free inside buzzer::play() / chirp(). Driven by
    // raw t_minus — independent of whether the digits are showing
    // dashes (a fresh, sane snapshot still tickles the audio path).
    if (have_t && prefs::current().button_sound) {
      const int8_t whole_sec_to_t0 = static_cast<int8_t>(
          (t_minus > 127) ? 127 : (t_minus < -128 ? -128 : t_minus));
      if (whole_sec_to_t0 != m_last_tick_sec) {
        if (whole_sec_to_t0 >= 1 && whole_sec_to_t0 <= 10) {
          buzzer::tick_click(880);  // ~A5, clean count tick
        } else if (whole_sec_to_t0 <= 0 && !m_ignition_fired) {
          buzzer::play(kIgnition,
                       sizeof(kIgnition) / sizeof(kIgnition[0]));
          m_ignition_fired = true;
        }
        m_last_tick_sec = whole_sec_to_t0;
      }
      // Re-arm ignition once we're well past T-0 so a sticky-after-
      // launch snapshot doesn't replay it.
      if (t_minus < -60) m_ignition_fired = false;
    } else {
      // No fresh data → invalidate edge tracker so the first tick
      // after recovery isn't lost to the "same second" coalesce.
      m_last_tick_sec = -1;
    }

    // ── Odometer state update for the six H/M/S digits ───────────
    char ds[6];
    if (data_valid) {
      // Absolute value for the H/M/S split — sign is conveyed
      // separately by the T-/T+ prefix below.
      const int32_t abs_t = (t_minus < 0) ? -t_minus : t_minus;
      const int32_t hh = abs_t / 3600;
      const int32_t mm = (abs_t % 3600) / 60;
      const int32_t ss = abs_t % 60;
      ds[0] = static_cast<char>('0' + (hh / 10));
      ds[1] = static_cast<char>('0' + (hh % 10));
      ds[2] = static_cast<char>('0' + (mm / 10));
      ds[3] = static_cast<char>('0' + (mm % 10));
      ds[4] = static_cast<char>('0' + (ss / 10));
      ds[5] = static_cast<char>('0' + (ss % 10));
      for (uint8_t i = 0; i < 6; ++i) {
        if (m_digits[i].cur == '-') {
          // First valid frame after a WAIT window — snap each cell
          // to the current digit with no animation so the operator
          // doesn't see six simultaneous dash→digit slides.
          m_digits[i].cur           = ds[i];
          m_digits[i].prev          = ds[i];
          m_digits[i].anim_start_ms = 0;
        } else if (ds[i] != m_digits[i].cur) {
          m_digits[i].prev          = m_digits[i].cur;
          m_digits[i].cur           = ds[i];
          // anim_start_ms == 0 means "no animation running"; bump
          // a millis() of exactly 0 (rare but possible across the
          // 49-day wrap) up by one to keep that sentinel safe.
          m_digits[i].anim_start_ms = (now_ms == 0u) ? 1u : now_ms;
        }
      }
    } else {
      // No data → dashes, no animation. Reset cur to '-' so the
      // next valid frame takes the snap-without-animation branch
      // above.
      for (uint8_t i = 0; i < 6; ++i) {
        m_digits[i].cur           = '-';
        m_digits[i].prev          = '-';
        m_digits[i].anim_start_ms = 0;
      }
    }

    // ── Digit row (built-in 6×8 mono font) ───────────────────────
    // Use Adafruit_GFX's built-in font so every glyph has the same
    // 6×8 cell — odometer slide math is the same `kDigitH = 8` for
    // every column, with no per-glyph kerning surprises. setFont
    // with nullptr selects it.
    matrix.setFont(nullptr);
    matrix.setTextSize(1);

    // Static prefix + separators. Prefix glyph picks one of three:
    //   "T-" — confirmed instantaneous t0 ahead, STATUS_DIM ink.
    //   "~T" — estimate ahead (publisher set t0_estimate=true,
    //          typically NET HR / DAY precision), STATUS_WARN ink so
    //          the operator can see at a glance the target is soft.
    //   "T+" — post-t0 (count-up), STATUS_DIM ink — launch already
    //          happened, the row is reporting elapsed time.
    //
    // Ink choice: STATUS_DIM (not HEADER_DIM) — HEADER_DIM is the
    // halo/glow slot and several themes (apollo_amber, blade_runner)
    // set it to 0x0000 as a "no halo" sentinel, which would make
    // the prefix invisible. STATUS_DIM is uniformly defined across
    // every theme as a low-value tint of the theme's body hue —
    // exactly the "present-but-recessed" role we want here.
    const bool estimate = data_valid && ls.t0_estimate && !post_t0;
    const uint16_t prefix_ink = estimate
        ? theme::ink(theme::Ink::STATUS_WARN)
        : theme::ink(theme::Ink::STATUS_DIM);
    matrix.setTextColor(prefix_ink);
    matrix.setCursor(kPrefixX, kDigitTop);
    matrix.print(post_t0 ? "T+" : (estimate ? "~T" : "T-"));
    matrix.setCursor(kColon1X, kDigitTop);
    matrix.print(':');
    matrix.setCursor(kColon2X, kDigitTop);
    matrix.print(':');

    // Digit ink: a steady STATUS_INFO until the final minute of a
    // countdown, then a 2 Hz pulse between ALERT and STATUS_WARN
    // for urgency. Post-t0 (count-up) suppresses the pulse — the
    // urgency moment has passed, the row is just reporting elapsed
    // time now. Held dashes use a dimmer ink so the row reads as
    // "awaiting data" instead of competing with live digits.
    uint16_t digit_ink;
    if (data_valid) {
      if (!post_t0 && t_minus < 60) {
        const bool fast = (now_ms % 500u) < 250u;  // 2 Hz
        digit_ink = fast ? theme::ink(theme::Ink::ALERT)
                         : theme::ink(theme::Ink::STATUS_WARN);
      } else {
        digit_ink = theme::ink(theme::Ink::STATUS_INFO);
      }
    } else {
      // Same reasoning as the prefix ink: STATUS_DIM is uniformly a
      // dim body-hue tint across themes, whereas HEADER_DIM is the
      // halo slot some themes zero out — using it here would make
      // the dashes invisible on apollo_amber / blade_runner.
      digit_ink = theme::ink(theme::Ink::STATUS_DIM);
    }
    matrix.setTextColor(digit_ink);
    for (uint8_t i = 0; i < 6; ++i) {
      draw_odo_digit(matrix, kDigitX[i], kDigitTop, m_digits[i], now_ms);
    }

    // ── Clip odometer slide-bleed ────────────────────────────────
    // Lower band: at p=1 the outgoing glyph's top sits at
    // kDigitTop + kDigitH, occupying y = kDigitTop+8 .. +15. Clobber
    // those rows back to black.
    matrix.fillRect(0, kDigitTop + kDigitH,
                    PANEL_WIDTH, kDigitH, 0x0000);
    // Upper band: at p=0 the incoming glyph's top sits at
    // kDigitTop − kDigitH (= 2 with kDigitTop=10), occupying
    // y = 2 .. 9. We clear the whole band 0..kDigitTop-1 in one
    // sweep — also wiping any prior-frame header dust — then redraw
    // the LNCH header on top below.
    matrix.fillRect(0, 0, PANEL_WIDTH, kDigitTop, 0x0000);

    // ── Header ───────────────────────────────────────────────────
    // "LNCH" pulse-dims at 1.5 s so the eye picks the row even when
    // the digits are static (e.g. holding on dashes). Drawn last so
    // it overpaints any upper-bleed digit pixels in rows 0..7. Uses
    // STATUS_DIM for the dim phase (not HEADER_DIM — see prefix
    // comment above; on apollo_amber HEADER_DIM=0x0000 would make
    // the header momentarily invisible every 1.5 s).
    const bool header_dim = (now_ms % 1500u) < 200u;
    const uint16_t header_ink     = theme::ink(theme::Ink::ALERT);
    const uint16_t header_dim_ink = theme::ink(theme::Ink::STATUS_DIM);
    gfx::draw_scene_header(matrix, "LNCH", 1, 0,
                           header_dim ? header_dim_ink : header_ink);

    // ── Typewriter info row (y=18..23) ───────────────────────────
    // Rotates one short fact about the selected launch at a time so
    // the operator gets MISSION / VEH / PAD / LIFTOFF / NET-or-
    // CONFIRMED / WIN / (post-t0) RESULT without each one stealing
    // a permanent slot on a 64\u00d732 panel. Blank when stale \u2014 the
    // marquee already carries "AWAITING SCHEDULE" so doubling up
    // would be noise.
    if (data_valid) {
      paint_typewriter_row(matrix, ls, post_t0, now_ms);
    } else {
      // Explicit clear so a leftover slide from the last fresh
      // frame doesn't sit there once we age out into the dash regime.
      matrix.fillRect(0, kInfoBandTop, PANEL_WIDTH, kInfoBandH, 0x0000);
    }

    // ── Marquee (mission description, bottom row) ────────────────
    // The y=24..31 band sits below the digit row's lower bleed mask
    // (y=18..23 stays black for breathing room) and is independent
    // of the odometer/header dirty regions, so we can paint it in a
    // single fillRect+print pair with no z-order surprises.
    //
    // Marquee text:
    //   * fresh + description present → mission description (HA-
    //     normalised + clipped to kDescriptionCap-1 chars).
    //   * fresh + description empty   → "PROV VEHICLE  MISSION @ PAD"
    //     tag line as the fallback identity.
    //   * stale snapshot              → "AWAITING SCHEDULE" so the
    //     freshness gate is explicit on the panel.
    //
    // No trailing pad — paint_marquee() bakes a full PANEL_WIDTH of
    // off-screen travel into the cycle, which gives a natural blank
    // gap between loops regardless of text length.
    char marquee[launch_state::kDescriptionCap + 16];
    if (fresh) {
      if (ls.description[0] != '\0') {
        snprintf(marquee, sizeof(marquee), "%s", ls.description);
      } else {
        snprintf(marquee, sizeof(marquee),
                 "%s %s  %s @ %s",
                 ls.provider, ls.vehicle, ls.mission, ls.pad_code);
      }
    } else {
      snprintf(marquee, sizeof(marquee), "AWAITING SCHEDULE");
    }
    paint_marquee(matrix, marquee, kMarqueeBaseline);
  }

  // Keep the standard corner clock chrome visible on this scene.
  bool wants_clock_chrome() const override { return true; }

private:
  // ── Odometer cell ─────────────────────────────────────────────
  // One per H/M/S digit. `cur` is the current value (rendered when
  // anim_start_ms==0). When the value changes mid-frame we copy
  // cur→prev, drop the new char into cur, and stamp anim_start_ms
  // so the next kAnimMs of render() draws both glyphs sliding down.
  // '-' is the sentinel for "no prior value" — used to suppress the
  // first-valid-frame animation when we come back from a WAIT.
  struct OdoDigit {
    char     cur           = '-';
    char     prev          = '-';
    uint32_t anim_start_ms = 0;  // 0 = not animating
  };

  // Draw one odometer cell. When anim_start_ms is set we draw the
  // new glyph offset upward by (kDigitH − offset) and the previous
  // glyph offset downward by offset, so the eye sees the new value
  // pushing the old value out of the bottom of the cell. Bleed
  // outside the cell is clipped by the render()-side mask bands.
  static void draw_odo_digit(Adafruit_Protomatter& matrix,
                             int16_t x, int16_t y_top,
                             OdoDigit& d, uint32_t now_ms) {
    if (d.anim_start_ms == 0u) {
      matrix.setCursor(x, y_top);
      matrix.print(d.cur);
      return;
    }
    const uint32_t elapsed = now_ms - d.anim_start_ms;
    if (elapsed >= kAnimMs) {
      d.anim_start_ms = 0u;
      matrix.setCursor(x, y_top);
      matrix.print(d.cur);
      return;
    }
    // Linear push-down. Integer math keeps NFR-1.3 happy.
    const int16_t offset = static_cast<int16_t>(
        (elapsed * static_cast<uint32_t>(kDigitH)) / kAnimMs);
    // New glyph: slides in from above (top at y_top−kDigitH at p=0).
    matrix.setCursor(x, static_cast<int16_t>(y_top - kDigitH + offset));
    matrix.print(d.cur);
    // Old glyph: slides down out of the cell (top reaches y_top+kDigitH at p=1).
    matrix.setCursor(x, static_cast<int16_t>(y_top + offset));
    matrix.print(d.prev);
  }

  // ── Typewriter info row (y=18..23) ─────────────────────────────
  // Build the rotation of short fact-slides for the current snapshot
  // and paint the active one with a typewriter-style char-by-char
  // reveal followed by a hold and a brief blank gap before the next.
  //
  // Slide list (built once per call, order is deliberate \u2014 mission
  // identity first, then logistics, then time/window context, then
  // post-t0 outcome):
  //   1. MISSION <name>      \u2014 the single most useful \"what is this\".
  //   2. VEH <vehicle>       \u2014 rocket identity independent of mission.
  //   3. PAD <code>          \u2014 short pad/launch-site tag.
  //   4. LIFTOFF HH:MM       — absolute local wall-clock t0 (only
  //                            when !post_t0; meaningless once liftoff
  //                            has happened).
  //   5. NET / CONFIRMED    — confidence tag (only when !post_t0).
  //   6. WIN +HH:MM         — launch window duration (only when
  //                            !post_t0 AND a window was published).
  //   7. <RESULT slide>     — post-t0 only:
  //                            result=-1 → "IN FLIGHT"
  //                            result=0  → "FAIL"
  //                            result=1  → "SUCCESS"
  //                            result=2  → "PARTIAL"
  //
  // Cadence: each slide gets `kSlideMs` total. Within that:
  //   0 .. N*kTypeMs   — reveal one char every kTypeMs ms.
  //   reveal..end-200  — hold the full string steady.
  //   end-200 .. end   — blank (clean visual erase before next slide).
  //
  // Stateless time slicing: we derive both the active slide index
  // and the within-slide phase from `now_ms` alone. The slide list
  // can change between frames (e.g. post-t0 flip drops slides 4–6
  // and adds the RESULT slide) and the cycle just re-anchors; the
  // visible jump is one frame and the operator typically can't tell
  // because the typewriter reveal immediately starts again.
  void paint_typewriter_row(Adafruit_Protomatter& matrix,
                            const launch_state::Snapshot& ls,
                            bool post_t0, uint32_t now_ms) const {
    // Build the slide list as an array of fixed-cap C-strings so
    // there's no heap allocation in the render loop. 8 slots covers
    // every possible combination (1–4 always + 5–7 only upcoming +
    // 8 only post-t0 — the upcoming and post-t0 sets are disjoint).
    constexpr uint8_t kSlideCap = 8;
    constexpr uint8_t kSlideTextCap = 24;  // 16 chars * 4 px = 64 px, +slack
    char        slides[kSlideCap][kSlideTextCap];
    theme::Ink  slide_ink[kSlideCap];
    uint8_t     slide_label_len[kSlideCap];  // 0 = no label (whole string in value ink)
    uint8_t     n_slides = 0;

    // add_slide() takes an explicit label (with trailing colon — or
    // empty for slides like MISSION / SUCCESS that are value-only)
    // and a printf-formatted value. The label is rendered at half
    // intensity to subordinate the chrome to the data; the value
    // gets the full theme ink. Storing the boundary as `label_len`
    // is what makes that two-tone rendering possible without
    // re-printing the same character twice.
    auto add_slide = [&](theme::Ink ink, const char* label,
                         const char* val_fmt, auto... val_args) {
      if (n_slides >= kSlideCap) return;
      char* dst = slides[n_slides];
      const int label_n = snprintf(dst, kSlideTextCap, "%s", label ? label : "");
      const uint8_t lbl_len = (label_n < 0)
          ? 0
          : (label_n >= kSlideTextCap ? kSlideTextCap - 1
                                       : static_cast<uint8_t>(label_n));
      snprintf(dst + lbl_len, kSlideTextCap - lbl_len, val_fmt, val_args...);
      slide_ink[n_slides]       = ink;
      slide_label_len[n_slides] = lbl_len;
      ++n_slides;
    };

    // Slide 1 — mission name in the theme's ACCENT ink (the "this is
    // the thing" pop). No label: some mission names are long and
    // the prefix burns half the row width before the actual name
    // starts; the LNCH header + T-/T+ row already make it
    // unambiguous what this string represents.
    if (ls.mission[0]     != '\0') add_slide(theme::Ink::ACCENT,      "",     "%s", ls.mission);
    if (ls.org[0]         != '\0') add_slide(theme::Ink::ACCENT,      "ORG:", "%s", ls.org);
    if (ls.vehicle[0]     != '\0') add_slide(theme::Ink::STATUS_INFO, "VEH:", "%s", ls.vehicle);
    // Location slide: country (or "STATE, USA" for US sites) shaped
    // by the publisher. No label — the value is geography and reads
    // self-evidently. Falls back to the compact `pad_code` only when
    // pad_country is absent (older publisher payloads); in that case
    // the PAD label disambiguates a 3-letter tag from a country.
    if (ls.pad_country[0] != '\0') {
      add_slide(theme::Ink::STATUS_INFO, "",     "%s", ls.pad_country);
    } else if (ls.pad_code[0] != '\0') {
      add_slide(theme::Ink::STATUS_INFO, "PAD:", "%s", ls.pad_code);
    }

    if (!post_t0) {
      // LIFTOFF HH:MM in local wall clock. t0_local_epoch is in the
      // HA-local frame so % 86400 gives seconds-since-midnight
      // directly — no tz math.
      const int32_t secs_of_day = ls.t0_local_epoch % 86400;
      const int hh = static_cast<int>(secs_of_day / 3600);
      const int mm = static_cast<int>((secs_of_day % 3600) / 60);
      add_slide(theme::Ink::STATUS_OK, "LIFTOFF:", "%02d:%02d", hh, mm);
      // NET = soft estimate (WARN ink), CONFIRMED = pinned t0 (OK
      // ink). Single-word slide — no label.
      add_slide(ls.t0_estimate ? theme::Ink::STATUS_WARN
                               : theme::Ink::STATUS_OK,
                "", "%s", ls.t0_estimate ? "NET" : "CONFIRMED");
      if (ls.t0_window_close_local_epoch != 0) {
        const int32_t win_s = ls.t0_window_close_local_epoch
                              - ls.t0_local_epoch;
        if (win_s > 0) {
          const int win_hh = static_cast<int>(win_s / 3600);
          const int win_mm = static_cast<int>((win_s % 3600) / 60);
          add_slide(theme::Ink::STATUS_INFO, "WIN:",
                    "+%02d:%02d", win_hh, win_mm);
        }
      }
    } else {
      const char* tag;
      theme::Ink  ink;
      switch (ls.result) {
        case  0: tag = "FAIL";      ink = theme::Ink::ALERT;       break;
        case  1: tag = "SUCCESS";   ink = theme::Ink::STATUS_OK;   break;
        case  2: tag = "PARTIAL";   ink = theme::Ink::STATUS_WARN; break;
        default: tag = "IN FLIGHT"; ink = theme::Ink::STATUS_WARN; break;  // -1 = no result yet
      }
      add_slide(ink, "", "%s", tag);
    }

    // Clear the band every frame — reveal repaints from index 0 each
    // call, so any leftover trailing chars from a longer prior slide
    // would otherwise stick around when a shorter slide replaces it.
    matrix.fillRect(0, kInfoBandTop, PANEL_WIDTH, kInfoBandH, 0x0000);
    if (n_slides == 0) return;

    // Time slicing: pick slide, then sub-phase.
    const uint32_t slot  = now_ms / kSlideMs;
    const uint8_t  idx   = static_cast<uint8_t>(slot % n_slides);
    const uint32_t phase = now_ms % kSlideMs;
    const char* text     = slides[idx];
    const uint16_t len   = static_cast<uint16_t>(strlen(text));
    const uint32_t reveal_ms = static_cast<uint32_t>(len) * kTypeMs;

    uint16_t chars_to_draw;
    // Cursor draw state. The trailing '_' glyph:
    //   * reveal phase  — drawn solid (no blink) so it reads as the
    //                     "write head" advancing one char at a time.
    //   * hold phase    — blinks at 2 Hz so the row reads as "the
    //                     terminal is waiting, the prompt is live".
    //   * blank gap     — suppressed (the whole band is dark).
    bool draw_cursor = false;
    if (phase < reveal_ms) {
      // Typewriter reveal: one char per kTypeMs.
      chars_to_draw = static_cast<uint16_t>(phase / kTypeMs);
      draw_cursor   = true;
    } else if (phase + kBlankMs >= kSlideMs) {
      // Trailing blank gap so the slide-to-slide transition reads
      // as an explicit erase, not as a jarring instant swap.
      chars_to_draw = 0;
      draw_cursor   = false;
    } else {
      chars_to_draw = len;  // hold the full string
      draw_cursor   = (now_ms % 600u) < 300u;  // ~1.7 Hz blink
    }
    if (chars_to_draw == 0 && !draw_cursor) return;

    matrix.setFont(&fonts::TomThumbShifted);
    matrix.setTextSize(1);
    const uint16_t value_color = theme::ink(slide_ink[idx]);
    // Half-intensity label color: bitshift each RGB565 channel so
    // luminance halves while hue stays close to the value ink. Fine
    // for label/value pairs (same hue, lower V); not the recipe for
    // "dim status" semantics elsewhere in the codebase — see the
    // theme/color-perception notes for why generic dim variants
    // need an HSV-aware drop instead.
    const uint16_t label_color = static_cast<uint16_t>(
        ((value_color & 0xF800) >> 1) & 0xF800u   // R: 5 bits >> 1
      | ((value_color & 0x07E0) >> 1) & 0x07E0u   // G: 6 bits >> 1
      | ((value_color & 0x001F) >> 1)             // B: 5 bits >> 1
    );
    matrix.setCursor(kInfoX, kInfoBaseline);
    const uint8_t lbl_len = slide_label_len[idx];
    // Label half (dim) — only the chars that are part of the label
    // AND have been revealed by the typewriter so far.
    const uint16_t lbl_draw = (chars_to_draw < lbl_len) ? chars_to_draw : lbl_len;
    if (lbl_draw > 0) {
      matrix.setTextColor(label_color);
      for (uint16_t i = 0; i < lbl_draw; ++i) matrix.print(text[i]);
    }
    // Value half (full ink).
    if (chars_to_draw > lbl_len) {
      matrix.setTextColor(value_color);
      for (uint16_t i = lbl_len; i < chars_to_draw; ++i) matrix.print(text[i]);
    }
    // Trailing cursor glyph at the current write-head column. Same
    // ink as the value (or label if still in the label segment) so
    // it reads as part of the line being typed rather than chrome.
    if (draw_cursor) {
      const int16_t cursor_x = static_cast<int16_t>(
          kInfoX + chars_to_draw * kTomThumbAdvance);
      matrix.setTextColor(chars_to_draw < lbl_len ? label_color : value_color);
      matrix.setCursor(cursor_x, kInfoBaseline);
      matrix.print('_');
    }
    // Restore built-in font so the next frame's odometer doesn't
    // inherit our font swap.
    matrix.setFont(nullptr);
  }

  // Paint the bottom-row marquee with the TomThumb 3×5 font (4 px
  // x-advance). Fixed-font choice is deliberate — the description
  // is prose, not data, so it stays visually subordinate to the
  // odometer regardless of which theme is active.
  //
  // Scroll model: the text enters from the right edge (x = PANEL_WIDTH),
  // scrolls left exactly one pixel per render call, and is considered
  // "out" once its tail has crossed the left edge (x + text_w <= 0).
  // The cycle then restarts from the right with no dead frame between
  // loops. Cycle length = PANEL_WIDTH + text_w, so:
  //   * short text (text_w < PANEL_WIDTH) gets a full right-to-left
  //     sweep rather than "snapping" in place;
  //   * long text (text_w > PANEL_WIDTH) clears the panel completely
  //     before re-entering — there is never a stale partial frame on
  //     screen during the wrap;
  //   * empty text wipes the band and bails (no spinning counter, no
  //     phantom cursor advance).
  //
  // Step cadence note: motion is driven by a per-frame counter
  // (`m_marquee_px`) rather than `now_ms / step`. Wall-clock-divided
  // stepping causes beat-frequency stutter whenever the divisor is
  // not commensurate with the actual frame interval — with a 32 ms
  // divisor and ~50 ms frames the sequence is +1, +2, +1, +2 px,
  // which the eye reads as jerk. Advancing exactly 1 px per render
  // call instead is what gives perceived smoothness: the eye
  // tracks spatial monotonicity, not absolute speed.
  void paint_marquee(Adafruit_Protomatter& matrix, const char* msg,
                     int16_t y_baseline) {
    // Clear the marquee band. TomThumb glyph height = 5 px, sits
    // above the baseline; we wipe an 8-row strip ending at the
    // panel bottom so cursor-y at y_baseline=31 sits the glyphs
    // flush with the bottom edge.
    matrix.fillRect(0, kMarqueeBandTop, PANEL_WIDTH, kMarqueeBandH, 0x0000);
    const int len = msg ? static_cast<int>(strlen(msg)) : 0;
    if (len == 0) {
      // Reset counter so a non-empty message later starts cleanly
      // from the right edge rather than mid-cycle.
      m_marquee_px = 0;
      return;
    }
    // Swap to TomThumb for this row only. Caller restores nothing
    // because there's nothing painted after the marquee.
    matrix.setFont(&fonts::TomThumbShifted);
    matrix.setTextSize(1);
    const int16_t text_w = static_cast<int16_t>(len * kTomThumbAdvance);
    // Full cycle: enter from x=PANEL_WIDTH, exit at x=-text_w.
    const int32_t cycle = static_cast<int32_t>(PANEL_WIDTH) + text_w;
    // Advance the persistent scroll counter exactly once per render
    // call. This is the linchpin of smooth motion — see the function
    // header for why time-division stepping stutters.
    m_marquee_px += 1;
    if (m_marquee_px >= cycle) m_marquee_px -= cycle;
    const int16_t x = static_cast<int16_t>(PANEL_WIDTH - m_marquee_px);
    matrix.setTextColor(theme::ink(theme::Ink::BODY));
    matrix.setCursor(x, y_baseline);
    matrix.print(msg);
    // Restore the built-in font so subsequent frames — which paint
    // the header / digits first — don't inherit our font swap.
    // (render() also re-asserts setFont(nullptr) before the digits,
    // belt + braces.)
    matrix.setFont(nullptr);
  }

  // ── Layout constants ──────────────────────────────────────────
  // "T-HH:MM:SS" is 10 cells × 6 px = 60 px wide; the panel is 64
  // px so we sit comfortably with 2 px of left/right margin.
  static constexpr int16_t  kDigitTop = 10;  // top of digit cell row
  static constexpr int16_t  kDigitH   = 8;   // built-in font cell height
  static constexpr uint32_t kAnimMs   = 180; // odometer flick duration
  // Bottom-row marquee: TomThumb 3×5 font rendered with its
  // baseline one row above the panel bottom so the 1-px descenders
  // of 'g', 'p', 'q', 'y' (yOffset = -4 → -3 after the +1 shift)
  // remain visible on row 31 instead of being clipped. Glyph body
  // occupies y=26..30, descender row at y=31; we clear a 7-row
  // band starting at kMarqueeBandTop to wipe both the glyph rows
  // and the 1-row gap that separates the marquee from the digit-
  // row lower bleed mask.
  static constexpr int16_t  kMarqueeBaseline = 30;  // cursor y
  static constexpr int16_t  kMarqueeBandTop  = 25;
  static constexpr int16_t  kMarqueeBandH    = 7;
  static constexpr int16_t  kTomThumbAdvance = 4;   // x-advance per char
  // Typewriter info row (y=18..24). TomThumb baseline at y=23 puts
  // the 5-px glyph body in y=19..23 and the 1-px descender row at
  // y=24, tucked one row above the marquee's wipe band (y=25..31).
  // Reveal cadence: 70 ms/char (~14 cps) is fast enough that a
  // 16-char string fills in ~1.1 s, slow enough that the eye tracks
  // the cursor; 2.5 s hold gives the operator time to read; 250 ms
  // blank gap separates slides visually so the next reveal doesn't
  // look like a continuation.
  static constexpr int16_t  kInfoBandTop  = 18;
  static constexpr int16_t  kInfoBandH    = 7;
  static constexpr int16_t  kInfoBaseline = 23;
  static constexpr int16_t  kInfoX        = 1;
  static constexpr uint32_t kTypeMs       = 70;
  static constexpr uint32_t kBlankMs      = 250;
  static constexpr uint32_t kSlideMs      = 4000;
  // 100 h ceiling; above this the format collapses (HH would need 3
  // digits) so we treat anything ≥ this as "out of range" → dashes.
  static constexpr int32_t  kMaxCountdownS = 100L * 3600L;
  // Post-t0 count-up ceiling. Mirrors the publisher's 30 min hold
  // (homeassistant/pyscript/observatory_publisher.py `_POST_T0_HOLD_S`)
  // with a small grace so a clock-skew edge case doesn't flip the
  // odometer to dashes while the publisher still considers the
  // mission selected. After this the scene falls back to dashes
  // and the next publish (≤ 10 min cadence) brings the next
  // upcoming launch into view.
  static constexpr int32_t  kMaxPostT0S    = 35L * 60L;
  static constexpr int16_t  kPrefixX = 2;    // 'T'
  static constexpr int16_t  kColon1X = 26;   // ':' between HH and MM
  static constexpr int16_t  kColon2X = 44;   // ':' between MM and SS
  // Per-digit x positions: HH, HH, MM, MM, SS, SS.
  static constexpr int16_t  kDigitX[6] = { 14, 20, 32, 38, 50, 56 };

  // 3-note rising sting at T-0. Theme-neutral in v1 (single melody
  // shared across themes — most retro-SF themes don't reference
  // rocketry, so a custom per-theme sting would feel forced). Total
  // duration is well under the FR-10.7 1500 ms ceiling so a sticky
  // launch snapshot whose t0 just elapsed can't stack ignitions if
  // we mis-detect the edge.
  static constexpr buzzer::Note kIgnition[] = {
    { 523, 120 },  // C5
    { 659, 120 },  // E5
    { 784, 320 },  // G5
  };

  // Per-second tick coalesce state.
  int8_t   m_last_tick_sec  = -1;   // most-recent T-minus integer second we acted on
  bool     m_ignition_fired = false;
  OdoDigit m_digits[6];             // HH, HH, MM, MM, SS, SS odometer cells
  // Marquee scroll counter — incremented exactly once per render
  // call so motion is always 1 px/frame regardless of frame
  // interval. See paint_marquee()'s header comment.
  int16_t  m_marquee_px     = 0;
};

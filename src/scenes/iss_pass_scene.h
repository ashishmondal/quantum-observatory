// iss_pass scene — split-panel ISS readout with typewriter telemetry.
//
// Layout (64×32):
//   left half  (x=0..31):
//     y= 0..7   "[ISS]" header in built-in 5×7 mono, brackets pulse
//     y= 9..14  Typewriter line 1 — altitude, Picopixel
//     y=17..22  Typewriter line 2 — crew, Picopixel
//     y=25..30  Typewriter line 3 — visibility readout, Picopixel:
//                 visible       → "VIS BBBxEE" (look-here in compass°
//                                 × altitude°), computed on-device
//                                 from the ISS sub-satellite point +
//                                 observer config
//                 not visible   → countdown "VIS IN 3D" / "5H" / "42M"
//                 stale/no data → "WAIT"
//   right half (x=32..63): artist-supplied iss.bmp.
//
// Each data line types out one Picopixel glyph at a time (~90 ms /
// char). A blinking 2-px cursor follows the typed prefix. As text
// arrives we paint a tight black rectangle behind only the typed
// glyphs so the readout stays legible against the bg artwork — the
// trailing portion of each row remains transparent (still showing
// the BMP) until those characters are typed.
//
// All animation state is derived from now_ms — no per-frame mutables,
// safe across scene re-init.
//
// Visibility logic (on-device, every frame): the station is
// "naked-eye visible from MY backyard right now" when
//   (a) ISS is in sunlight                — iss_state.sunlit
//   (b) Observer is in twilight or darker — sun::compute() ≤ -6°
//   (c) ISS is above the observer horizon — iss_geom::look_angles() ≥ 0°
// HA pushes only the raw upstream pass-throughs; the firmware does
// the AND every frame from config.h LATITUDE_DEG/LONGITUDE_DEG +
// tod::now() UTC. No HA template logic, no firmware TLE math.
//
// Per FR-9.3 this scene opts OUT of the corner clock chrome.
//
// (added in phase 7.1; rewritten with typewriter effect in 7.1+;
//  visibility + look-angles moved on-device in 7.1++)

#pragma once

#include <cstring>
#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "backgrounds/image_palette_bg.h"
#include "bitmaps/_index.h"
#include "config.h"
#include "gfx_text.h"
#include "iss_geometry.h"
#include "iss_state.h"
#include "scene.h"
#include "sun_position.h"
#include "theme.h"
#include "time_of_day.h"

class IssPassScene : public Scene {
public:
  const char* name() const override { return "iss_pass"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    for (int i = 0; i < kImageRegistryCount; ++i) {
      const ImageEntry& e = kImageRegistry[i];
      if (e.name != nullptr && std::strcmp(e.name, "iss") == 0) {
        m_bg.set(e);
        return;
      }
    }
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // ── Background art ───────────────────────────────────────────────
    m_bg.render(matrix, now_ms);

    // ── Snapshot + on-device geometry ────────────────────────────────
    iss_state::Snapshot iss;
    const bool iss_fresh = iss_state::get(now_ms, &iss);

    // Derive look-angles + visibility every frame. Cheap (~150 µs
    // for the geometry + ~30 µs per trig in sun::compute), called
    // once per frame at most. Falls back to "not visible" when we
    // don't have the inputs to decide (no fresh snapshot, no valid
    // RTC time).
    bool visible       = false;
    bool have_position = false;
    int  bearing_deg   = 0;
    int  iss_elev_deg  = 0;
    if (iss_fresh) {
      const iss_geom::LookAngles la = iss_geom::look_angles(
          LATITUDE_DEG, LONGITUDE_DEG,
          iss.iss_lat_deg, iss.iss_lon_deg,
          static_cast<float>(iss.altitude_km));
      // Round to nearest int; clamp bearing into [0,359] in case
      // 359.6 rounds to 360.
      int b = static_cast<int>(la.azimuth_deg + 0.5f);
      if (b >= 360) b -= 360;
      if (b <    0) b += 360;
      bearing_deg  = b;
      iss_elev_deg = static_cast<int>(la.elevation_deg + 0.5f);
      have_position = true;

      // Visibility AND. Need a valid local time to compute the sun
      // elevation; without it we conservatively call it not visible
      // (better to say "VIS IN ..." until the RTC catches up than to
      // claim a visible pass that might not exist).
      const tod::Reading r = tod::now(now_ms);
      if (r.valid && iss.sunlit && la.elevation_deg >= 0.0f) {
        const int32_t utc_epoch = r.local_epoch
            - static_cast<int32_t>(LOCAL_TZ_OFFSET_MIN) * 60;
        const sun::Position sp =
            sun::compute(utc_epoch, LATITUDE_DEG, LONGITUDE_DEG);
        if (sp.altitude_deg <= -6.0f) {
          visible = true;
        }
      }
    }

    // ── Build the three typewriter lines ────────────────────────────
    // ALT comes from MQTT (rounded km). CREW is the only optionally-
    // absent field on the wire — when HA hasn't pushed astros.json
    // yet (typical on cold boot) we render "CREW ?" rather than
    // fabricating a number, matching the FR-1.4 spirit that
    // downstream readers never see invented data.
    char line1[16], line2[16], line3[16];
    if (iss_fresh) {
      snprintf(line1, sizeof(line1), "ALT %uKM",
               static_cast<unsigned>(iss.altitude_km));
    } else {
      snprintf(line1, sizeof(line1), "ALT ?");
    }
    if (iss_fresh && iss.have_crew) {
      snprintf(line2, sizeof(line2), "CREW %u",
               static_cast<unsigned>(iss.crew_count));
    } else {
      snprintf(line2, sizeof(line2), "CREW ?");
    }
    format_visibility_line(iss_fresh, visible, have_position,
                           bearing_deg, iss_elev_deg,
                           iss, now_ms, line3, sizeof(line3));

    const char* lines[3] = { line1, line2, line3 };
    const uint8_t lens[3] = {
      static_cast<uint8_t>(strlen(line1)),
      static_cast<uint8_t>(strlen(line2)),
      static_cast<uint8_t>(strlen(line3)),
    };
    constexpr uint8_t kBaselineY[3] = { 14, 22, 30 };  // Picopixel baselines

    // ── Typewriter schedule (all derived from now_ms) ───────────────
    constexpr uint32_t kCharMs   = 90;    // per-glyph type rate
    constexpr uint32_t kPauseMs  = 600;   // pause after a line completes
    constexpr uint32_t kHoldMs   = 2500;  // hold after all 3 done before restart

    const uint32_t span1 = lens[0] * kCharMs + kPauseMs;
    const uint32_t span2 = lens[1] * kCharMs + kPauseMs;
    const uint32_t span3 = lens[2] * kCharMs + kPauseMs;
    const uint32_t total = span1 + span2 + span3 + kHoldMs;
    const uint32_t t     = now_ms % total;

    const uint32_t starts[3] = { 0u, span1, span1 + span2 };

    // Per-line typed count + which line currently shows the cursor.
    uint8_t typed[3] = { 0, 0, 0 };
    int8_t  active   = -1;
    for (int i = 0; i < 3; ++i) {
      if (t < starts[i]) {
        typed[i] = 0;
      } else {
        const uint32_t local = t - starts[i];
        const uint32_t typing_dur = lens[i] * kCharMs;
        if (local < typing_dur) {
          typed[i] = static_cast<uint8_t>(local / kCharMs);
          active = static_cast<int8_t>(i);
        } else {
          typed[i] = lens[i];
        }
      }
    }
    const bool cursor_on = ((now_ms / 280u) & 1u) == 0u;

    // ── Header: pulsing identity-bracketed name in built-in 5×7 mono ─
    // ISS scene identity = STATUS_OK (green = "healthy / overhead‑able");
    // pulse-low rides the matching STATUS_OK_DIM so a future theme can
    // re-tone both ends together. Brackets / halo / BLOCK_BARS routing
    // owned by theme via gfx::draw_scene_header (T.7a).
    const bool header_dim = (now_ms % 1500u) < 200u;
    const uint16_t header_ink     = theme::ink(theme::Ink::STATUS_OK);
    const uint16_t header_dim_ink = theme::ink(theme::Ink::STATUS_OK_DIM);
    gfx::draw_scene_header(matrix, "ISS", 1, 0,
                           header_dim ? header_dim_ink : header_ink);

    // ── Three typewriter lines (Picopixel) ──────────────────────
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    // Per-font baseline correction (THEME.md §2.3): nostromo_green's
    // BODY = TomThumb sits one row above Picopixel/Org_01.
    const int8_t kBodyDy = theme::baseline_y_shift(theme::FontRole::BODY);

    const uint16_t kVisibleInk = theme::ink(theme::Ink::STATUS_OK);    // overhead
    const uint16_t kWaitInk    = theme::ink(theme::Ink::STATUS_STALE); // no fresh data
    const uint16_t kCountInk   = theme::ink(theme::Ink::STATUS_WARN);  // countdown
    const uint16_t kDataInk    = theme::ink(theme::Ink::STATUS_INFO);  // altitude
    const uint16_t kCrewInk    = theme::ink(theme::Ink::ACCENT_MAGENTA);

    for (int i = 0; i < 3; ++i) {
      if (typed[i] == 0 && active != i) continue;  // nothing to draw yet

      // Build the partial string (up to typed[i] glyphs).
      char prefix[16];
      const uint8_t n = typed[i];
      memcpy(prefix, lines[i], n);
      prefix[n] = '\0';
      const int16_t by = static_cast<int16_t>(kBaselineY[i] + kBodyDy);

      // Measure the partial string so the black background hugs the
      // glyphs and the cursor lines up exactly. Picopixel has variable
      // glyph widths; getTextBounds gives the authoritative pixel box.
      int16_t  bx, by_unused;
      uint16_t bw, bh;
      uint16_t prefix_px = 0;
      if (n > 0) {
        matrix.getTextBounds(prefix, 1, by, &bx, &by_unused, &bw, &bh);
        prefix_px = bw;
      }

      // Black backdrop: 1 px padding above + below the glyph cap.
      // Picopixel glyphs are 5 px tall; row band y=baseline-5..baseline.
      // Use fillRect so the trailing (untyped) portion of the row stays
      // transparent / shows the BMP.
      const int16_t bg_x = 0;
      const int16_t bg_y = static_cast<int16_t>(by - 6);
      const int16_t bg_h = 5;
      const int16_t bg_w = (n > 0) ? static_cast<int16_t>(prefix_px + 2) : 0;
      // Add room for the cursor (2 px wide + 1 px gap) when this line
      // is actively typing.
      int16_t bg_w_total = bg_w;
      if (active == i && cursor_on) bg_w_total += 4;
      if (bg_w_total > 0) {
        matrix.fillRect(bg_x, bg_y + 2, bg_w_total - 1, bg_h, 0x0000);  // universal background
      }

      // Pick the colour: VIS line uses VISIBLE/countdown/WAIT hue once
      // typed; the CREW line is magenta; ALT line is cyan.
      uint16_t ink = kDataInk;
      if (i == 1) ink = kCrewInk;
      if (i == 2 && typed[i] > 0) {
        if      (visible)    ink = kVisibleInk;
        else if (!iss_fresh) ink = kWaitInk;
        else                 ink = kCountInk;
      }

      if (n > 0) {
        matrix.setTextColor(ink);
        matrix.setCursor(1, by);
        matrix.print(prefix);
      }

      // Cursor — small filled rect just past the last glyph. Drawn in
      // the line's ink colour (or cyan if nothing typed yet) so it
      // reads as the same beam.
      if (active == i && cursor_on) {
        const int16_t cx = static_cast<int16_t>(1 + prefix_px + 1);
        matrix.fillRect(cx, bg_y + 1, 3, 7, kDataInk);
      }
    }

    // matrix.show() is called by loop1() (phase 3.5.2).
  }

private:
  // Format the line-3 visibility readout into `out`. One of:
  //   "VIS BBBxEE"   — overhead now, with on-device-computed
  //                    bearing (compass °) and elevation (°)
  //   "VIS"          — overhead now but we don't yet have a position
  //                    (defensive fallback; should not normally fire
  //                     because `visible` requires a position)
  //   "VIS IN 3D"    — next pass ≥ 1 day out (days only)
  //   "VIS IN 5H"    — next pass ≥ 1 hour but < 1 day (hours only)
  //   "VIS IN 12M"   — next pass < 1 hour (minutes; floors to 1 near 0)
  //   "VIS SOON"     — countdown collapsed past zero before HA refreshed
  //   "WAIT"         — no fresh snapshot from HA
  // The countdown projects from set_at_ms via the millis() delta so
  // the panel ticks down between MQTT pushes (FR-9.5 spirit: writers
  // anchor, readers smooth).
  static void format_visibility_line(bool fresh, bool visible,
                                     bool have_position,
                                     int bearing_deg, int elev_deg,
                                     const iss_state::Snapshot& iss,
                                     uint32_t now_ms,
                                     char* out, size_t cap) {
    if (visible) {
      if (have_position) {
        // Clamp elevation into [0,90] for a stable two-digit max
        // render width — geometry can return 90+epsilon at zenith.
        int e = elev_deg;
        if (e <  0) e =  0;
        if (e > 90) e = 90;
        snprintf(out, cap, "VIS %03dx%d", bearing_deg, e);
      } else {
        snprintf(out, cap, "VIS");
      }
      return;
    }
    if (!fresh) {
      snprintf(out, cap, "WAIT");
      return;
    }
    // Project the live remaining count. Use signed math so a snapshot
    // we've already passed (HA late on the next push) clamps to 0
    // rather than wrapping the uint32_t.
    const uint32_t elapsed_ms = now_ms - iss.set_at_ms;
    const int32_t  remain_s   = static_cast<int32_t>(iss.seconds_until_next)
                              - static_cast<int32_t>(elapsed_ms / 1000u);
    if (remain_s <= 0) {
      // Pass is imminent or HA hasn't refreshed yet — show "SOON" so
      // the reader knows the data is real but the precise count has
      // collapsed. Avoids a "VIS IN 0M" that lies for hours.
      snprintf(out, cap, "VIS SOON");
      return;
    }
    const uint32_t r = static_cast<uint32_t>(remain_s);
    if (r >= 86400u) {
      // ≥ 1 day → days only. Round to nearest day so the readout
      // doesn't sit on "3D" for 23 hours and then jump to "2D" for
      // one tick — nearest-rounding spreads the transition.
      const uint32_t days = (r + 43200u) / 86400u;
      snprintf(out, cap, "VIS IN %luD", static_cast<unsigned long>(days));
    } else if (r >= 3600u) {
      // ≥ 1 hour → hours only, nearest-hour rounded for the same
      // "don't sit on a stale digit" reason.
      const uint32_t hours = (r + 1800u) / 3600u;
      snprintf(out, cap, "VIS IN %luH", static_cast<unsigned long>(hours));
    } else {
      // < 1 hour → minutes only. Floor for a truthful tick-down, but
      // clamp to 1 in the final 60 s so we never advertise "VIS IN 0M"
      // (that lie is reserved for the SOON branch above).
      uint32_t mins = r / 60u;
      if (mins == 0) mins = 1;
      snprintf(out, cap, "VIS IN %luM", static_cast<unsigned long>(mins));
    }
  }

  ImagePaletteBg m_bg;
};

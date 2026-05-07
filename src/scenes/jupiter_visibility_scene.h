// jupiter_visibility scene — split-panel Jupiter readout with
// typewriter telemetry (phase 7.3).
//
// Layout (64×32):
//   left half  (x=0..31):
//     y= 0..7   "[JUP]" header in built-in 5×7 mono, brackets pulse
//     y= 9..14  Typewriter line 1 — apparent magnitude, Picopixel
//     y=17..22  Typewriter line 2 — distance (AU), Picopixel
//     y=25..30  Typewriter line 3 — visibility readout, Picopixel:
//                 visible       → "VIS BBBxEE" (look-here in compass°
//                                 × altitude°), straight from the
//                                 wire (HA already provides the
//                                 ephemeris look-angles)
//                 below horizon → "BELOW"
//                 above horizon
//                 but daylight  → "IN <IAU>" — host constellation
//                                 (3-letter IAU code, e.g. "IN TAU")
//                 stale/no data → "WAIT"
//   right half (x=32..63): artist-supplied jupiter.bmp.
//
// Each data line types out one Picopixel glyph at a time (~90 ms /
// char) — same rhythm as iss_pass + moon_phase so the dashboard feels
// consistent. A blinking 2-px cursor follows the typed prefix; we
// paint a tight black backdrop only behind the typed glyphs so the
// trailing portion of each row stays transparent (still showing the
// BMP) until those characters are typed.
//
// Visibility derivation (on-device, every frame): Jupiter is "naked-
// eye visible from MY backyard right now" when
//   (a) Jupiter is above the observer horizon — wire elevation_deg ≥ 0
//   (b) Observer is in twilight or darker      — sun::compute() ≤ -6°
// (Jupiter is always sunlit — planets shine by reflected light — so
// there is no `sunlit` field on the wire, unlike the ISS payload.)
//
// HA pushes only the raw upstream pass-throughs (bearing, elevation,
// optional magnitude / distance). The firmware does the AND every
// frame from config.h LATITUDE_DEG/LONGITUDE_DEG + tod::now() UTC.
// Mirrors the FR-14 architecture chosen for iss_pass — no HA template
// logic, no firmware ephemeris math. (added in phase 7.3)
//
// Per FR-9.3 this scene KEEPS the corner clock chrome — the [JUP]
// header lives in the top-left (~x=1..30) and the chrome HH:MM lives
// in the top-right (~x=44..63), no collision. Same trade-off the
// moon_phase scene makes: chrome overlays the corner of the bmp.

#pragma once

#include <cstring>
#include <stdio.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>

#include "backgrounds/image_palette_bg.h"
#include "bitmaps/_index.h"
#include "config.h"
#include "jupiter_state.h"
#include "scene.h"
#include "stars.h"
#include "sun_position.h"
#include "time_of_day.h"

class JupiterVisibilityScene : public Scene {
public:
  const char* name() const override { return "jupiter_visibility"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    for (int i = 0; i < kImageRegistryCount; ++i) {
      const ImageEntry& e = kImageRegistry[i];
      if (e.name != nullptr && std::strcmp(e.name, "jupiter") == 0) {
        m_bg.set(e.palette, e.pixels, e.regions, e.region_count);
        return;
      }
    }
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // ── Background art ───────────────────────────────────────────────
    m_bg.render(matrix, now_ms);

    // ── Snapshot + on-device twilight check ──────────────────────────
    jupiter_state::Snapshot jup;
    const bool jup_fresh = jupiter_state::get(now_ms, &jup);

    // Vis state machine: WAIT (no data) → BELOW (below horizon) →
    // DAY (above horizon, sun too high) → VIS (above horizon, dark
    // enough). We need a valid local time to decide DAY vs VIS;
    // without it we conservatively call it DAY rather than claim a
    // visible target that may be washed out.
    VisState vis = VisState::WAIT;
    if (jup_fresh) {
      if (jup.elevation_deg < 0) {
        vis = VisState::BELOW;
      } else {
        vis = VisState::DAY;  // default until we prove darkness
        const tod::Reading r = tod::now(now_ms);
        if (r.valid) {
          const int32_t utc_epoch = r.local_epoch
              - static_cast<int32_t>(LOCAL_TZ_OFFSET_MIN) * 60;
          const sun::Position sp =
              sun::compute(utc_epoch, LATITUDE_DEG, LONGITUDE_DEG);
          if (sp.altitude_deg <= -6.0f) {
            vis = VisState::VISIBLE;
          }
        }
      }
    }

    // ── Build the three typewriter lines ────────────────────────────
    // MAG / DIST are the only two optional wire fields — render "?"
    // when absent, matching the FR-1.4 spirit that downstream readers
    // never see invented data. VIS line is built by the helper.
    char line1[16], line2[16], line3[16];
    if (jup_fresh && jup.have_magnitude) {
      // magnitude_x10 is fixed-point to keep float math out of the
      // render loop (NFR-1.3). Negative magnitudes are common for
      // Jupiter (~-2.9 at opposition); preserve sign + 1 decimal.
      const int32_t m = jup.magnitude_x10;
      const int32_t whole = m / 10;
      const int32_t frac  = m < 0 ? -m % 10 : m % 10;
      if (m < 0 && whole == 0) {
        snprintf(line1, sizeof(line1), "MAG -0.%ld",
                 static_cast<long>(frac));
      } else {
        snprintf(line1, sizeof(line1), "MAG %ld.%ld",
                 static_cast<long>(whole), static_cast<long>(frac));
      }
    } else {
      snprintf(line1, sizeof(line1), "MAG ?");
    }
    if (jup_fresh && jup.have_distance) {
      const uint32_t d = jup.distance_au_x10;
      snprintf(line2, sizeof(line2), "DIST %lu.%luAU",
               static_cast<unsigned long>(d / 10u),
               static_cast<unsigned long>(d % 10u));
    } else {
      snprintf(line2, sizeof(line2), "DIST ?");
    }
    format_visibility_line(jup_fresh, vis, jup, line3, sizeof(line3));

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

    // ── Header: pulsing [JUP] in built-in 5×7 mono ──────────────────
    // Amber-orange tones — Jupiter's banded look on the BMP is
    // peach/cream; the header echoes that family while staying
    // distinct from the green ISS [ISS] and grey moon [MOON].
    const bool header_dim = (now_ms % 1500u) < 200u;
    constexpr uint16_t kHeaderInk = 0xFD20;  // bright amber
    constexpr uint16_t kHeaderDim = 0x6A00;  // deep amber
    matrix.setFont(nullptr);
    matrix.setTextSize(1);
    matrix.setTextColor(header_dim ? kHeaderDim : kHeaderInk);
    matrix.setCursor(1, 0);
    matrix.print("[JUP]");

    // ── Three typewriter lines (Picopixel) ──────────────────────────
    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);

    constexpr uint16_t kVisibleInk = 0x07E0;  // green — banner when overhead + dark
    constexpr uint16_t kBelowInk   = 0x630C;  // dim grey — below horizon
    constexpr uint16_t kDayInk     = 0xFD20;  // amber — above horizon, in daylight
    constexpr uint16_t kWaitInk    = 0xC100;  // dim amber-red — no fresh data
    constexpr uint16_t kMagInk     = 0x07FF;  // cyan — apparent magnitude
    constexpr uint16_t kDistInk    = 0xFE60;  // peach — distance, echoes Jupiter's bands

    for (int i = 0; i < 3; ++i) {
      if (typed[i] == 0 && active != i) continue;  // nothing to draw yet

      char prefix[16];
      const uint8_t n = typed[i];
      memcpy(prefix, lines[i], n);
      prefix[n] = '\0';

      // Measure typed prefix (Picopixel is variable-width).
      int16_t  bx, by;
      uint16_t bw, bh;
      uint16_t prefix_px = 0;
      if (n > 0) {
        matrix.getTextBounds(prefix, 1, kBaselineY[i], &bx, &by, &bw, &bh);
        prefix_px = bw;
      }

      // Black backdrop, 1 px padding above + below the glyph cap.
      const int16_t bg_x = 0;
      const int16_t bg_y = static_cast<int16_t>(kBaselineY[i] - 6);
      const int16_t bg_h = 5;
      const int16_t bg_w = (n > 0) ? static_cast<int16_t>(prefix_px + 2) : 0;
      int16_t bg_w_total = bg_w;
      if (active == i && cursor_on) bg_w_total += 4;
      if (bg_w_total > 0) {
        matrix.fillRect(bg_x, bg_y + 2, bg_w_total - 1, bg_h, 0x0000);
      }

      // Per-line ink. VIS line picks its hue from the state machine
      // once it has at least one glyph typed; MAG/DIST stay on their
      // identity colours.
      uint16_t ink = kMagInk;
      if (i == 1) ink = kDistInk;
      if (i == 2 && typed[i] > 0) {
        switch (vis) {
          case VisState::VISIBLE: ink = kVisibleInk; break;
          case VisState::BELOW:   ink = kBelowInk;   break;
          case VisState::DAY:     ink = kDayInk;     break;
          case VisState::WAIT:    ink = kWaitInk;    break;
        }
      }

      if (n > 0) {
        matrix.setTextColor(ink);
        matrix.setCursor(1, kBaselineY[i]);
        matrix.print(prefix);
      }

      // Cursor — small filled rect just past the last glyph.
      if (active == i && cursor_on) {
        const int16_t cx = static_cast<int16_t>(1 + prefix_px + 1);
        matrix.fillRect(cx, bg_y + 1, 3, 7, kMagInk);
      }
    }

    // matrix.show() is called by loop1() (phase 3.5.2).
  }

  // FR-9.3: keep the corner clock chrome. [JUP] header is left-
  // aligned (~x=1..30); chrome HH:MM is right-aligned (~x=44..63);
  // they share row 0..6 without overlapping. Mirrors moon_phase.
  bool wants_clock_chrome() const override { return true; }

private:
  enum class VisState : uint8_t { WAIT, BELOW, DAY, VISIBLE };

  // Format the line-3 visibility readout into `out`. One of:
  //   "VIS BBBxEE"  — overhead + dark, pointing string
  //   "BELOW"       — Jupiter below the observer horizon
  //   "IN <IAU>"    — above horizon but sun too high (washed out);
  //                   shows the host constellation (3-letter IAU
  //                   code) since constellation_index is required
  //                   on the wire
  //   "WAIT"        — no fresh snapshot from HA
  static void format_visibility_line(bool fresh, VisState vis,
                                     const jupiter_state::Snapshot& jup,
                                     char* out, size_t cap) {
    if (!fresh) {
      snprintf(out, cap, "WAIT");
      return;
    }
    switch (vis) {
      case VisState::VISIBLE: {
        // Clamp elevation into [0,90] for a stable two-digit max
        // render width — wire could theoretically push 90 exactly
        // and we want a deterministic glyph count.
        int e = jup.elevation_deg;
        if (e <  0) e =  0;
        if (e > 90) e = 90;
        // Bearing is uint-ranged 0..359 on the wire; modulo for
        // belt-and-suspenders.
        int b = jup.bearing_deg;
        if (b < 0)    b = 0;
        if (b > 359)  b = 359;
        snprintf(out, cap, "VIS %03dx%d", b, e);
        break;
      }
      case VisState::BELOW: snprintf(out, cap, "BELOW"); break;
      case VisState::DAY: {
        // constellation_index is required on the wire; the parser
        // rejects payloads without it, so a fresh snapshot is
        // guaranteed to carry a valid 0..87 index. Upper-case the
        // 3-letter IAU code to match the dashboard's all-caps
        // text style.
        const char* iau =
            constellations_iau::kCatalog[jup.constellation_index].iau;
        char up[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 3 && iau[i] != '\0'; ++i) {
          char c = iau[i];
          if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
          up[i] = c;
        }
        snprintf(out, cap, "IN %s", up);
        break;
      }
      case VisState::WAIT:  snprintf(out, cap, "WAIT");  break;
    }
  }

  ImagePaletteBg m_bg;
};

// moon_phase scene — split-panel lunar readout (sticky, priority 2).
//
// Layout (64×32):
//   left half  (x=0..31):
//     y= 0..7   "[MOON]" header in built-in 5×7 mono, brackets pulse
//     y= 9..14  Typewriter line 1 — phase name (Picopixel)
//     y=17..22  Typewriter line 2 — "ILL %d%%"
//     y=25..30  Typewriter line 3 — "AGE %dD"
//   right half (x=32..63): artist-supplied moon.bmp with phase
//     shading overlaid on the lit/unlit regions of the moon disc.
//
// The page is monochromatic — every text colour is sampled from the
// moon palette so the foreground tracks the artwork's warm-grey hue.
//
// Phase shading: rather than draw a procedural disc, we render the
// flash-resident moon.bmp first, then overdraw the unlit pixels of
// the moon shape with a dim earthshine tone (also from the palette).
// Membership "is this pixel part of the moon?" comes from comparing
// the bitmap index against the surrounding background index — keeps
// the shading silhouetted on the artwork instead of a perfect circle.
//
// Phase computed from the RTC (FR-9.5). Synodic period 29.530588 d
// from a known new moon (2000-01-06 18:14 UTC, unix epoch 947182440).
// The few-hour offset between RTC local time and true UTC is
// negligible at this resolution (~0.01 of phase fraction worst case).
//
// Per FR-9.3 this scene opts OUT of the corner clock chrome — the
// header readout would overlap.
//
// (added in phase 7.2; switched to bg-art + phase-overlay in 7.2+)

#pragma once

#include <cstring>
#include <math.h>
#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "backgrounds/image_palette_bg.h"
#include "bitmaps/_index.h"
#include "bitmaps/moon.h"
#include "config.h"
#include "gfx_text.h"
#include "moon_state.h"
#include "scene.h"
#include "theme.h"
#include "time_of_day.h"
#include "typewriter.h"

class MoonPhaseScene : public Scene {
public:
  const char* name() const override { return "moon_phase"; }

  // Top-right HH:MM chrome fits next to the [MOON] header (header is
  // left-aligned, chrome is right-aligned — they share row 0..6
  // without overlapping; the moon disc sits below at y≥3).

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    for (int i = 0; i < kImageRegistryCount; ++i) {
      const ImageEntry& e = kImageRegistry[i];
      if (e.name != nullptr && std::strcmp(e.name, "moon") == 0) {
        m_bg.set(e);
        return;
      }
    }
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // ── Background art (moon.bmp full-frame) ────────────────────────
    m_bg.render(matrix, now_ms);

    // ── Source: HA push (preferred) or local synodic-month math ─────
    // observatory/moon delivers HA's authoritative phase + illum +
    // age + name. We treat it as fresh for kFreshMs (12 h); past
    // that, fall back to the on-board calculation so the panel keeps
    // a sensible readout if HA goes dark.
    constexpr int32_t kNewMoonRef = 947182440;   // 2000-01-06 18:14 UTC
    constexpr int32_t kSynodicSec = 2551443;     // 29.530588 d

    float       phase_frac = 0.0f;
    int         age_days   = 0;
    int         illum_pct  = 0;
    bool        data_known = false;
    const char* mqtt_name  = nullptr;

    moon_state::Snapshot snap;
    if (moon_state::get(now_ms, &snap) && snap.valid) {
      phase_frac = snap.phase_frac;
      age_days   = snap.age_d;
      illum_pct  = snap.illum_pct;
      data_known = true;
      if (snap.name[0] != '\0') mqtt_name = snap.name;
    } else {
      tod::Reading r = tod::now(now_ms);
      if (r.valid) {
        int32_t age_sec = (r.local_epoch - kNewMoonRef) % kSynodicSec;
        if (age_sec < 0) age_sec += kSynodicSec;
        phase_frac = static_cast<float>(age_sec)
                   / static_cast<float>(kSynodicSec);
        age_days   = age_sec / 86400;
        const float cos_p = cosf(6.2831853f * phase_frac);
        illum_pct = static_cast<int>((1.0f - cos_p) * 50.0f + 0.5f);
        if (illum_pct < 0)   illum_pct = 0;
        if (illum_pct > 100) illum_pct = 100;
        data_known = true;
      }
    }

    // ── Phase overlay on the moon shape ─────────────────────────────
    if (data_known) shade_moon(matrix, phase_frac);

    // ── Build the three typewriter lines ────────────────────────────
    char line1[12], line2[12], line3[12];
    const char* phase_str = data_known
        ? (mqtt_name ? mqtt_name : phase_name(phase_frac))
        : "--";
    snprintf(line1, sizeof(line1), "%s", phase_str);
    if (data_known) {
      snprintf(line2, sizeof(line2), "ILL %d%%", illum_pct);
      snprintf(line3, sizeof(line3), "AGE %dD",  age_days);
    } else {
      snprintf(line2, sizeof(line2), "ILL --");
      snprintf(line3, sizeof(line3), "AGE --");
    }

    const char* lines[3] = { line1, line2, line3 };
    const uint8_t lens[3] = {
      static_cast<uint8_t>(strlen(line1)),
      static_cast<uint8_t>(strlen(line2)),
      static_cast<uint8_t>(strlen(line3)),
    };
    constexpr uint8_t kBaselineY[3] = { 14, 22, 30 };

    // ── Typewriter schedule (all derived from now_ms) ───────────────
    const typewriter::Schedule tw = typewriter::compute(now_ms, lens);
    const uint8_t* typed     = tw.typed;
    const int8_t   active    = tw.active;
    const bool     cursor_on = tw.cursor_on;

    // ── Custom monochrome ramp (FG region is ours per FR-12.1) ──────
    // PHASE bar inverts (black on bright) so it pops as a "current
    // state" badge; ILL/AGE present dim labels next to bright values
    // for at-a-glance readout.
    //
    // Header / phase-bar / cursor inks are moon-palette greys
    // (0xEF5D peak highlight, 0x4228 deep shadow) chosen to match
    // the moon BMP's tonality — scene-internal, NOT theme-owned
    // (CODING_PRACTICES §4). Label / value ride the shared
    // theme::LABEL / theme::VALUE roles since the 20%/80% white
    // split is a generic typewriter pattern other scenes will reuse.
    constexpr uint16_t kHeaderInk  = 0xEF5D;  // peak highlight (from moon palette)
    constexpr uint16_t kHeaderDim  = 0x4228;  // deep shadow grey
    constexpr uint16_t kPhaseBar   = 0xEF5D;  // bright bar behind PHASE
    constexpr uint16_t kPhaseInk   = 0x0000;  // black glyphs on the bar (universal)
    const     uint16_t kLabelInk   = theme::ink(theme::Ink::LABEL);
    const     uint16_t kValueInk   = theme::ink(theme::Ink::VALUE);
    constexpr uint16_t kCursorInk  = 0xEF5D;  // peak — pops on every line

    // ── Header: pulsing identity-bracketed name in built-in 5×7 mono ─
    // MOON identity = scene-internal moon-grey (kHeaderInk/kHeaderDim
    // above) — not a STATUS_* role; brackets / halo / BLOCK_BARS
    // routing owned by theme via gfx::draw_scene_header (T.7a).
    const bool header_dim = (now_ms % 1500u) < 200u;
    gfx::draw_scene_header(matrix, "MOON", 6, 0,
                           header_dim ? kHeaderDim : kHeaderInk);

    // ── Three typewriter lines (Picopixel) ──────────────────────────
    // PHASE (i=0): black glyphs on a bright bar — reads as a current-
    // state badge. ILL/AGE (i=1,2): dim 4-char label "ILL "/"AGE "
    // followed by bright value, so the eye finds the number first.
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    constexpr uint8_t kLabelLen = 4;  // "ILL " / "AGE " (incl. trailing space)
    constexpr int16_t kTextX    = 6;  // global +5 shift from the panel edge

    for (int i = 0; i < 3; ++i) {
      if (typed[i] == 0 && active != i) continue;

      const uint8_t n = typed[i];
      const int16_t by = static_cast<int16_t>(kBaselineY[i]);

      // Measure the typed prefix once (authoritative width for
      // backdrop + cursor placement). Picopixel is variable-width.
      char prefix[12];
      memcpy(prefix, lines[i], n);
      prefix[n] = '\0';
      int16_t  bx, by_unused;
      uint16_t bw, bh;
      uint16_t prefix_px = 0;
      if (n > 0) {
        matrix.getTextBounds(prefix, kTextX, by, &bx, &by_unused, &bw, &bh);
        prefix_px = bw;
      }

      // Backdrop band: 5 px tall, 1 px above the glyph cap.
      const int16_t bg_y = static_cast<int16_t>(by - 6);
      const int16_t bg_h = 5;
      const int16_t bg_w = (n > 0) ? static_cast<int16_t>(prefix_px + 2) : 0;
      int16_t bg_w_total = bg_w;
      if (active == i && cursor_on) bg_w_total += 4;
      // PHASE inverts (black-on-bright bar) and is expanded 1 px on
      // every side for a chunkier badge feel; ILL/AGE keep black
      // and the tight 5-px backdrop.
      const uint16_t backdrop = (i == 0) ? kPhaseBar : 0x0000;
      if (bg_w_total > 0) {
        if (i == 0) {
          matrix.fillRect(kTextX - 2, bg_y + 1,
                          bg_w_total - 1 + 2, bg_h + 2, backdrop);
        } else {
          matrix.fillRect(kTextX - 1, bg_y + 2,
                          bg_w_total - 1, bg_h, backdrop);
        }
      }

      if (n > 0) {
        if (i == 0) {
          // PHASE bar — single colour (black on bright).
          matrix.setTextColor(kPhaseInk);
          matrix.setCursor(kTextX, by);
          matrix.print(prefix);
        } else {
          // ILL / AGE — split label vs value at kLabelLen.
          // While typing the label itself (n <= kLabelLen) all
          // typed glyphs are dim; once the value starts the
          // dim/bright handoff kicks in.
          const uint8_t label_n = (n < kLabelLen) ? n : kLabelLen;
          char lbl[kLabelLen + 1];
          memcpy(lbl, lines[i], label_n);
          lbl[label_n] = '\0';
          matrix.setTextColor(kLabelInk);
          matrix.setCursor(kTextX, by);
          matrix.print(lbl);

          if (n > kLabelLen) {
            // Measure label width to know where the value starts.
            int16_t lbx, lby;
            uint16_t lbw, lbh;
            matrix.getTextBounds(lbl, kTextX, by, &lbx, &lby, &lbw, &lbh);
            char val[12];
            const uint8_t val_n = static_cast<uint8_t>(n - kLabelLen);
            memcpy(val, lines[i] + kLabelLen, val_n);
            val[val_n] = '\0';
            matrix.setTextColor(kValueInk);
            matrix.setCursor(kTextX + lbw, by);
            matrix.print(val);
          }
        }
      }

      if (active == i && cursor_on) {
        const int16_t cx = static_cast<int16_t>(kTextX + prefix_px + 1);
        matrix.fillRect(cx, bg_y + 1, 3, 7, kCursorInk);
      }
    }

    // matrix.show() is called by loop1() (phase 3.5.2).
  }

private:
  // 8-phase bucketing centred on the canonical phase angles
  // (each bucket is 1/8 of a synodic month wide).
  static const char* phase_name(float p) {
    if (p < 1.0f/16.0f || p >= 15.0f/16.0f) return "NEW";
    if (p < 3.0f/16.0f)                     return "WAX CRES";
    if (p < 5.0f/16.0f)                     return "FIRST Q";
    if (p < 7.0f/16.0f)                     return "WAX GIB";
    if (p < 9.0f/16.0f)                     return "FULL";
    if (p < 11.0f/16.0f)                    return "WAN GIB";
    if (p < 13.0f/16.0f)                    return "LAST Q";
                                            return "WAN CRES";
  }

  // Overdraw the unlit pixels of the moon shape with a dim earthshine
  // tone. Membership = "this pixel of moon.bmp differs from the
  // surrounding background index" — silhouettes the shading on the
  // artwork's natural shape instead of forcing a perfect disc.
  //
  // Geometry per moon.bmp authoring: disc is ~20 px wide × 21 px high
  // centred near (cx=49, cy=14). Use R=10 sphere model for the
  // terminator half-width.
  static void shade_moon(Adafruit_Protomatter& matrix, float phase_frac) {
    constexpr int      cx       = 49;
    constexpr int      cy       = 14;
    constexpr int      R        = 10;
    constexpr int      R2       = R * R;
    constexpr uint8_t  kBgIdx   = 11;       // surround-fill index in moon.bmp
    constexpr uint16_t kUnlit   = 0x2104;   // dim warm grey from kMoonPalette

    // T in [-1,+1]: terminator x as a fraction of the disc half-width
    // at the centre row. Waxing (p<0.5): pixel is bright iff dx ≥ T·w.
    // Waning: bright iff dx ≤ -T·w.
    const float T = cosf(6.2831853f * phase_frac);
    const bool  waxing = phase_frac < 0.5f;

    for (int dy = -11; dy <= 10; ++dy) {
      const int py = cy + dy;
      if (py < 0 || py >= 32) continue;
      // Per-row half-width from the sphere model (clamp to [-R,R]).
      const int dy_clip = (dy < -R) ? -R : ((dy > R) ? R : dy);
      const int w2 = R2 - dy_clip * dy_clip;
      const int w  = (w2 <= 0) ? 1
                               : static_cast<int>(sqrtf(static_cast<float>(w2)));
      const int term_x = static_cast<int>(
          T * static_cast<float>(w) + (T >= 0.0f ? 0.5f : -0.5f));

      for (int dx = -R - 1; dx <= R + 1; ++dx) {
        const int px = cx + dx;
        if (px < 32 || px >= 64) continue;  // right-half only
        const uint8_t idx = kMoonPixels[py * 64 + px];
        if (idx == kBgIdx) continue;        // not part of the moon shape
        const bool bright = waxing ? (dx >= term_x) : (dx <= -term_x);
        if (!bright) matrix.drawPixel(px, py, kUnlit);
      }
    }
  }

  ImagePaletteBg m_bg;
};

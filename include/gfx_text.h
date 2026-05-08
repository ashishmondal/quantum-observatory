// Text drawing helpers that compose with arbitrary backgrounds.
//
// draw_text_halo() draws each glyph in a halo color at the 8 surrounding
// 1-pixel offsets first, then re-draws it in the foreground color on top.
// The halo creates a destructive outline that guarantees legibility over
// animated bg layers (FR-3.3) without needing to know the bg's pixels.
//
// Cost: 9× the glyph rasterisation per call. At our typical line lengths
// (≤14 chars per FR-4.4) this is ~100 µs at 30 FPS — negligible against
// the 200+ FPS headroom we measured.
//
// draw_header() / draw_body() are 2-line layout primitives (FR-3.3,
// FR-4.3): they pick the font, compute centred X via getTextBounds, fix
// the baseline Y in each band (header 0–7, body 9–16 per PLAN 3.4), and
// halo-draw. Scenes pass a string and trust the layout.
//
// (added in phase 3.3; layout primitives in phase 3.4)

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/Picopixel.h>

#include "config.h"
#include "sky_snapshot.h"
#include "time_of_day.h"

namespace gfx {

// Draws `str` at (x, y) — same coordinate semantics as matrix.setCursor +
// matrix.print, i.e. baseline for GFX fonts, top-left for the built-in
// font. Caller is responsible for setFont/setTextSize before calling.
inline void draw_text_halo(Adafruit_Protomatter& matrix,
                           int16_t x, int16_t y,
                           const char* str,
                           uint16_t fg,
                           uint16_t halo) {
  // 8 cardinal + diagonal offsets. Order doesn't matter — they all get
  // overdrawn by the fg pass.
  static const int8_t kOffsets[8][2] = {
    {-1, -1}, {0, -1}, {1, -1},
    {-1,  0},          {1,  0},
    {-1,  1}, {0,  1}, {1,  1},
  };

  matrix.setTextColor(halo);
  for (auto& o : kOffsets) {
    matrix.setCursor(x + o[0], y + o[1]);
    matrix.print(str);
  }

  matrix.setTextColor(fg);
  matrix.setCursor(x, y);
  matrix.print(str);
}

// Internal: measure `str` in the font currently set on `matrix` and
// horizontally centre on the panel. Returns the X coordinate to pass to
// draw_text_halo / setCursor (same baseline semantics).
inline int16_t centered_x(Adafruit_Protomatter& matrix, const char* str) {
  int16_t  x1, y1;
  uint16_t w, h;
  matrix.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (PANEL_WIDTH - static_cast<int16_t>(w)) / 2 - x1;
  if (x < 0) x = 0;       // FR-4.4 truncation is Director's job; just clamp.
  return x;
}

// Draw a centred, haloed header line.
//   - Font: FreeSansBold9pt7b (placeholder for Space Mono Bold, FR-4.1).
//   - Baseline Y = 14 → caps span ~rows 2–13, halo to ~rows 1–14.
//     FreeSansBold9pt7b caps are ~12 px tall (much taller than the
//     spec'd 7-px Space Mono Bold), which is why the header eats most
//     of the panel for now. Will tighten when 8.6 swaps the real font.
inline void draw_header(Adafruit_Protomatter& matrix, const char* str,
                        uint16_t fg = 0xFFFF, uint16_t halo = 0x0000) {
  matrix.setFont(&FreeSansBold9pt7b);
  matrix.setTextSize(1);
  draw_text_halo(matrix, centered_x(matrix, str), 14, str, fg, halo);
}

// Draw a centred, haloed body line, just under the header band.
//   - Font: Picopixel (placeholder for Silkscreen 5x7, FR-4.1).
//   - Baseline Y = 22 → glyphs span ~rows 18–22, halo to ~rows 17–23.
//     Sits in the lower third, below the (currently oversized) header.
inline void draw_body(Adafruit_Protomatter& matrix, const char* str,
                      uint16_t fg = 0xFFFF, uint16_t halo = 0x0000) {
  matrix.setFont(&Picopixel);
  matrix.setTextSize(1);
  draw_text_halo(matrix, centered_x(matrix, str), 22, str, fg, halo);
}

// Always-on small clock readout — top-right corner, Picopixel HH:MM with
// halo. Renders "--:--" until tod has been set (FR-9.6). Designed to be
// called by the main render loop AFTER the active scene's foreground but
// BEFORE matrix.show(), so every scene gains the chrome without editing
// any scene file (FR-9.2, FR-9.3). Scenes opt out via
// Scene::wants_clock_chrome() — only the giant clock should.
//
// Cost: ~5 chars × 9 halo passes × Picopixel glyph (~3×5 px). Cheap
// against our ~69 FPS budget. (added in phase 3.5.2)
inline void draw_clock_chrome(Adafruit_Protomatter& matrix, uint32_t now_ms,
                              uint16_t fg = 0xFFFF, uint16_t halo = 0x0000) {
  uint8_t hh = 0, mm = 0;
  const bool valid = tod::now_hhmm(now_ms, &hh, &mm);

  char buf[6]; // "HH:MM" + NUL
  if (valid) {
    // No %02u for plain unsigned char on AVR-ish toolchains; cast to int.
    snprintf(buf, sizeof(buf), "%02d:%02d", static_cast<int>(hh),
                                            static_cast<int>(mm));
  } else {
    // FR-9.6: never show a guessed time before first sync.
    buf[0] = '-'; buf[1] = '-'; buf[2] = ':';
    buf[3] = '-'; buf[4] = '-'; buf[5] = '\0';
  }

  matrix.setFont(&Picopixel);
  matrix.setTextSize(1);

  int16_t x1, y1; uint16_t w, h;
  matrix.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  // Top-right with 1 px halo margin from each edge. Picopixel baseline
  // sits ~5 px below the top of caps; y=5 puts caps in rows 1..5, halo
  // extends to rows 0..6 — within the panel.
  int16_t x = PANEL_WIDTH - static_cast<int16_t>(w) - 1 - x1;
  if (x < 0) x = 0;
  draw_text_halo(matrix, x, 5, buf, fg, halo);
}

// Always-on micro sun-arc indicator — single pixel along the top edge
// (row 0) at the column corresponding to the live sun azimuth. Driven
// by the FR-16.5 sky_snapshot so this costs nothing on the render path
// beyond a snapshot read + one drawPixel. Below-horizon → no pixel;
// near horizon → dim warm; above horizon → bright amber.
//
// The azimuth→x mapping mirrors sky_bg_render::draw() exactly so the
// chrome dot lines up with the sun disc when the sky bg is showing.
// Skipped when the snapshot is invalid (pre-RTC-sync) or when the
// rendering scene has opted out of chrome — same opt-out as the
// clock readout, since both share the top edge.
//
// (added in phase D.6 — FR-16.5 chrome micro-indicator)
inline void draw_sun_arc_chrome(Adafruit_Protomatter& matrix) {
  sky_snapshot::Snapshot snap;
  sky_snapshot::read(&snap);
  if (!snap.valid) return;

  // Mirror sky_bg_render::draw()'s linear az→x mapping (az 60°=0,
  // 180°=31.5, 300°=63). Outside [45°, 315°] the sun is "behind us"
  // for the south-facing panel — no indicator.
  const float az = snap.sun_azimuth_deg;
  if (az < 45.0f || az > 315.0f) return;
  const float xf = (az - 60.0f) * (63.0f / 240.0f);
  int x = static_cast<int>(xf + 0.5f);
  if (x < 0) x = 0;
  if (x > PANEL_WIDTH - 1) x = PANEL_WIDTH - 1;

  // Top-left corner is reserved by the chrome clock readout (which
  // we draw on the right) — row 0 is otherwise free. Use a warm
  // amber that stays legible against any sky-bg gradient. Below
  // horizon (the snap.observer_dark window or sub-horizon) we just
  // skip; the indicator is meant for "where IS the sun in the sky",
  // not "where would it be if it were up" — that's what the
  // sky_timelapse debug scene is for.
  if (snap.sun_altitude_deg < -3.0f) return;

  // Two-tier brightness: bright amber when sun > 5° (well up), warm
  // dim near horizon. Matches the disc gradient in sky_bg.
  const uint16_t bright = 0xFE60;  // amber (R31 G19 B0)
  const uint16_t warm   = 0x8200;  // deep orange-red (R16 G16 B0)
  const uint16_t c = (snap.sun_altitude_deg > 5.0f) ? bright : warm;
  matrix.drawPixel(x, 0, c);
}

}  // namespace gfx

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
#include <stdio.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/Picopixel.h>

#include "config.h"
#include "theme.h"
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

// Draw a left-anchored, haloed header line.
//   - Font: theme::FontRole::HEADER (per-theme — Press Start 2P / NokiaFC22
//     / Pixel Operator). Routing through theme means a theme switch
//     re-fonts every header-band caller without per-scene edits
//     (FR-15.3 / FR-15.4).
//   - X = 2 (uniform left margin across every scene + theme).
//   - Baseline Y = 15 (1 px lower than the prior y=14 — the extra row
//     gives ascender-heavy theme headers like NokiaFC22 / Pixel Operator
//     a touch more breathing room from the panel top).
inline void draw_header(Adafruit_Protomatter& matrix, const char* str,
                        uint16_t fg = 0xFFFF, uint16_t halo = 0x0000) {
  matrix.setFont(theme::font(theme::FontRole::HEADER));
  matrix.setTextSize(1);
  // Baseline Y = 15 (1 px lower than the prior y=14 — the extra row
  // gives ascender-heavy theme headers like NokiaFC22 / Pixel Operator
  // a touch more breathing room from the panel top).
  draw_text_halo(matrix, /*x=*/2, /*y=*/15, str, fg, halo);
}

// Draw a centred, haloed body line, just under the header band.
//   - Font: theme::FontRole::BODY (per-theme).
//   - Baseline Y = 22 → glyphs span ~rows 18–22, halo to ~rows 17–23.
inline void draw_body(Adafruit_Protomatter& matrix, const char* str,
                      uint16_t fg = 0xFFFF, uint16_t halo = 0x0000) {
  matrix.setFont(theme::font(theme::FontRole::BODY));
  matrix.setTextSize(1);
  draw_text_halo(matrix, centered_x(matrix, str), /*y=*/22, str, fg, halo);
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

  // Chrome HH:MM rides BODY — small per-theme readable text. A theme
  // switch re-fonts the corner readout for free (FR-15.3 / FR-15.4).
  matrix.setFont(theme::font(theme::FontRole::BODY));
  matrix.setTextSize(1);

  int16_t x1, y1; uint16_t w, h;
  matrix.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  // Top-right with 1 px halo margin from each edge. Picopixel baseline
  // sits ~5 px below the top of caps; y=5 puts caps in rows 1..5, halo
  // extends to rows 0..6 — within the panel.
  int16_t x = PANEL_WIDTH - static_cast<int16_t>(w) - 1 - x1;
  if (x < 0) x = 0;
  draw_text_halo(matrix, x, /*y=*/5, buf, fg, halo);
}

// ── Theme layout-hint primitives (FR-15 / THEME.md §3.4) ─────────────
//
// Each is cheap and gated by `theme::has(Hint)`. Called from a single
// post-scene, pre-`matrix.show()` site so themes that declare a hint
// get it for free across every scene without per-scene refactor. Themes
// that don't declare the hint pay nothing — `theme::has()` is one byte
// load + one bit test.
//
// (added in phase T.7)

// 1-px outer rectangle in HEADER ink. Used by BLADE_RUNNER + LCARS
// FRAME_BORDER (THEME.md §2.4 / §2.5). Two hlines + two vlines is
// 4·64 = 256 pixel writes — negligible against the panel's 2048-pixel
// frame budget.
inline void draw_theme_frame(Adafruit_Protomatter& matrix) {
  const uint16_t c = theme::ink(theme::Ink::HEADER);
  matrix.drawFastHLine(0, 0,                   PANEL_WIDTH,  c);
  matrix.drawFastHLine(0, PANEL_HEIGHT - 1,    PANEL_WIDTH,  c);
  matrix.drawFastVLine(0, 0,                   PANEL_HEIGHT, c);
  matrix.drawFastVLine(PANEL_WIDTH - 1, 0,     PANEL_HEIGHT, c);
}

// Every-other-row dimmer overlay — Nostromo CRT scanlines hint
// (THEME.md §2.2). Rather than re-blend the underlying pixel (which
// Protomatter doesn't support reading back, see CODING_PRACTICES §10),
// stamp a sparse pattern of dim phosphor on alternating rows. Reads as
// a CRT scanline veneer at 64 px width. Cheap: 16 hlines.
inline void draw_theme_scanlines(Adafruit_Protomatter& matrix) {
  // Pull the dim-phosphor color from the active theme so a future
  // scanline-using theme (other than Nostromo) inherits the right hue.
  const uint16_t dim = theme::ink(theme::Ink::DIVIDER);
  for (int16_t y = 1; y < PANEL_HEIGHT; y += 2) {
    // Stipple every 4th column at half-row stride — visible texture
    // without overpowering the underlying scene.
    for (int16_t x = 0; x < PANEL_WIDTH; x += 4) {
      matrix.drawPixel(x, y, dim);
    }
  }
}

// LCARS-style colored block bars in lieu of bracket glyphs (THEME.md
// §2.5 BLOCK_BARS). Draws "▮ NAME ▮" with two filled 3×7 orange blocks
// flanking a header string. Scenes that want LCARS's full look call
// this *instead* of prepending bracket strings to a snprintf'd header.
// y is the top of the header band (rows y..y+6 used). Caller is
// responsible for setting the font + body-ink before calling.
//
// Returns the X coordinate where the header baseline glyphs should be
// drawn (immediately after the left block, with a 1-px gap), so the
// caller can chain a `draw_text_halo` if it wants a haloed name.
inline int16_t draw_theme_block_header(Adafruit_Protomatter& matrix,
                                       const char* name,
                                       int16_t y) {
  const uint16_t accent = theme::ink(theme::Ink::ACCENT);
  // Measure the name in the currently-set font to centre everything.
  int16_t  x1, y1; uint16_t w, h;
  matrix.getTextBounds(name, 0, 0, &x1, &y1, &w, &h);
  const int16_t kBlockW = 3;
  const int16_t kBlockH = 7;
  const int16_t total = kBlockW + 1 + static_cast<int16_t>(w) + 1 + kBlockW;
  int16_t x = (PANEL_WIDTH - total) / 2;
  if (x < 0) x = 0;
  matrix.fillRect(x, y, kBlockW, kBlockH, accent);
  matrix.fillRect(x + total - kBlockW, y, kBlockW, kBlockH, accent);
  return x + kBlockW + 1 - x1;  // text baseline X — caller owns the Y/baseline.
}

// Typewriter-scene identity header — "[NAME]" in the active theme's
// brackets, drawn in the active theme's HEADER font (Press Start 2P /
// NokiaFC22 / Pixel Operator) with halo from theme::ink(HEADER_HALO) so
// NEON_OUTLINE themes (Vectrex/BR) glow and bracket-only themes pay
// no visible halo cost (Apollo/Nostromo halo is 0x0000 = black on the
// already-black panel band). Under BLOCK_BARS themes (LCARS) brackets
// are empty strings and the helper instead centers
// `draw_theme_block_header()` blocks flanking NAME, ignoring (x).
//
// (x, y) follows matrix.print() top-left semantics matching the
// existing typewriter scene call sites (iss_pass / jupiter_visibility
// / moon_phase / constellation_now). The helper translates to the
// font's baseline internally so callers don't have to know whether
// the active HEADER font is built-in (top-left origin) or a GFXfont
// (baseline origin) — the theme can swap freely.
//
// (added in phase T.7a — FR-15.3 bracket / halo / block-bars routing;
//  T.x extended to consume theme::font(HEADER) so identity headers
//  retypeset on theme switch alongside body / chrome.)
inline void draw_scene_header(Adafruit_Protomatter& matrix,
                              const char* name,
                              int16_t /*x_unused*/, int16_t y,
                              uint16_t fg) {
  matrix.setFont(theme::font(theme::FontRole::HEADER));
  matrix.setTextSize(1);

  // Uniform layout policy (T.x polish): every scene header is
  // left-anchored at x=2 and dropped 1 px from the caller-supplied
  // top-left y. This makes the header band visually consistent across
  // scenes/themes and frees callers from font-baseline math — they
  // pass y=0 and the helper translates to the GFX baseline.
  const int16_t kHeaderX = 2;
  const int16_t y_top    = y + 1;

  if (theme::has(theme::Hint::BLOCK_BARS)) {
    // BLOCK_BARS path: left-anchor the block + name composition at
    // x=kHeaderX so it lines up with bracketed themes. Block is 3 px
    // wide, then 1 px gap, then the name glyphs.
    int16_t  bx1, by1; uint16_t bw, bh;
    matrix.getTextBounds(name, 0, 0, &bx1, &by1, &bw, &bh);
    const int16_t baseline_y = y_top - by1;
    const uint16_t accent = theme::ink(theme::Ink::ACCENT);
    const int16_t kBlockW = 3;
    const int16_t kBlockH = 7;
    matrix.fillRect(kHeaderX, y_top, kBlockW, kBlockH, accent);
    matrix.setTextColor(fg);
    matrix.setCursor(kHeaderX + kBlockW + 1 - bx1, baseline_y);
    matrix.print(name);
    return;
  }

  // Bracketed path — theme owns the bracket glyphs (FR-15.3).
  char buf[16];
  snprintf(buf, sizeof(buf), "%s%s%s",
           theme::bracket_open(), name, theme::bracket_close());
  const uint16_t halo = theme::ink(theme::Ink::HEADER_HALO);
  // Translate top-left y → baseline y for GFXfont rendering. For the
  // built-in 5×7 font (legacy callers) y1 == 0 so this is a no-op.
  int16_t  bx1, by1; uint16_t bw, bh;
  matrix.getTextBounds(buf, 0, 0, &bx1, &by1, &bw, &bh);
  const int16_t baseline_y = y_top - by1;
  draw_text_halo(matrix, kHeaderX, baseline_y, buf, fg, halo);
}

// Umbrella decoration pass. Called once after the scene + chrome have
// drawn but before `matrix.show()`, so theme-level decorations layer
// on top of everything. Hints not declared by the active theme cost
// one byte load + one branch each.
inline void draw_theme_decorations(Adafruit_Protomatter& matrix) {
  if (theme::has(theme::Hint::SCANLINES))    draw_theme_scanlines(matrix);
  if (theme::has(theme::Hint::FRAME_BORDER)) draw_theme_frame(matrix);
  // CURSOR_BLOCK / NEON_OUTLINE / BLOCK_BARS / GIANT_DIGIT_GHOST are
  // per-scene primitives (consumed by header / digit drawing call
  // sites), not full-panel decorations — no global pass for them.
}

}  // namespace gfx

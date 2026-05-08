// Graphics smoke-test scene.
//
// One screen that exercises every pixel pipeline we care about and
// reports its own frame rate so any regression is visible at a glance.
// Brought up over MQTT with payload {"scene_id":"gfx_test"} on
// observatory/scene.
//
// Two roles bundled into one scene:
//
//   1. Palette / FPS smoke test (FR-12.6) — top half of the panel
//      cycles a nebula BG ramp and three FG brightness ramps so any
//      regression in palette::bg/fg or refresh stability shows up
//      immediately. A 1 s-window FPS readout + jitter-witness pixel
//      catches frame-rate irregularity by eye.
//
//   2. Theme coverage (FR-15.8 / phase T.9) — bottom half cycles
//      every theme on a fixed cadence (kThemeStepMs) and renders a
//      compact battery that exercises every theme::Ink role and every
//      Hint overlay. Theme switching is driven from Core 0 in main.cpp's
//      loop() (writer-side rule per CODING_PRACTICES §3) — this scene
//      only reads via theme::current() / theme::ink() / theme::has().
//      A single 60 s capture covers all five themes with hint demos.
//
// Layout (64x32 panel, no clock chrome — wants_clock_chrome() = false):
//
//   rows  0..7  cycling nebula BG palette
//   row   8     DIVIDER ink hline (theme-driven separator)
//   rows  9..14 three FG ramps (white/blue/amber, 2 rows each)
//   row  15     DIVIDER ink hline
//   rows 16..23 theme HEADER text via gfx::draw_scene_header() — engages
//               brackets / BLOCK_BARS / NEON_OUTLINE per active theme.
//               A 2x4 CURSOR_BLOCK in ACCENT_MAGENTA appears after the
//               header when the active theme declares Hint::CURSOR_BLOCK
//               (Nostromo), blinking on a 1 Hz cadence.
//   rows 24..25 25 ink swatches — 2 px wide × 2 px tall, packed
//               left-to-right covering all theme::Ink roles. The strip
//               re-tones with the active theme so a regression in any
//               Ink mapping is visible at a glance.
//   rows 26..29 Picopixel readout in BODY ink: FPS NN | <THEME 3-CHAR>
//               + a small "8" ghost-behind-"1" demo using GHOST + BODY
//               inks (mimics GIANT_DIGIT_GHOST at small scale; the
//               full-size ghost lives in giant_clock_scene).
//   row  30     uptime "T NNs" in BODY ink
//   row  31     red jitter pixel hopping each frame
//
// FRAME_BORDER + SCANLINES are applied as a global post-pass via
// gfx::draw_theme_decorations() in the chrome layer, so they overlay
// gfx_test automatically when the active theme declares them.
//
// Self-contained — no globals from main.cpp beyond the theme cycler.
//
// FR-15.3 exemption (CODING_PRACTICES §4): the upper half deliberately
// uses raw palette literals — that's the *palette smoothness* test and
// must stay theme-independent so a theming regression cannot mask a
// rendering regression. The lower half consumes theme:: directly to
// validate the theming layer itself. Each region's intent is documented
// inline.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>

#include "color_palette.h"
#include "config.h"
#include "gfx_text.h"
#include "scenes/scene.h"
#include "theme.h"

class GfxTestScene : public Scene {
public:
  const char* name() const override { return "gfx_test"; }
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    (void)matrix;
    m_init_ms       = 0;
    m_initialised   = false;
    m_last_tick_ms  = 0;
    m_palette_shift = 0;
    m_frames_in_window = 0;
    m_fps_display      = 0;
    m_window_start_ms  = 0;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (!m_initialised) {
      m_init_ms          = now_ms;
      m_last_tick_ms     = now_ms;
      m_window_start_ms  = now_ms;
      m_initialised      = true;
    }

    // --- Palette-cycle phase advance -------------------------------------
    // Wall-clock-driven so the visual speed is consistent across whatever
    // FPS the test is actually achieving (the whole point of the diag).
    const uint32_t dt = now_ms - m_last_tick_ms;
    m_last_tick_ms = now_ms;
    // ~24 steps/sec — fast enough to see motion clearly, slow enough that
    // any frame drop reads as a visible stutter.
    m_palette_shift += static_cast<uint16_t>((dt * 24u) / 1000u);

    // --- FPS sampling ----------------------------------------------------
    m_frames_in_window++;
    const uint32_t window_age = now_ms - m_window_start_ms;
    if (window_age >= 1000u) {
      // (frames * 1000) / window_age — integer math, no divide-by-zero
      // because window_age >= 1000.
      m_fps_display    = (m_frames_in_window * 1000u) / window_age;
      m_frames_in_window = 0;
      m_window_start_ms  = now_ms;
    }

    // --- Draw ------------------------------------------------------------
    matrix.fillScreen(0x0000);

    // ── Upper half: palette / FPS smoke test (FR-12.6) ──────────────────
    // Theme-independent by design — see file header.

    // Top strip: cycling nebula. Map x in 0..63 → BG index 0..189 (each
    // column = 3 palette steps), then add the global shift. The whole
    // strip looks uniform per frame but the colors flow sideways.
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < PANEL_WIDTH; ++x) {
        const uint16_t idx = static_cast<uint16_t>(x * 3);  // 0..189
        matrix.drawPixel(x, y,
            palette::bg(palette::Id::NEBULA_CLOUDS, idx, m_palette_shift));
      }
    }

    // FG ramps. Brightness across the panel width — 64 px maps 1:1 onto
    // the 64-entry FG region, so each pixel is its own brightness step.
    draw_fg_ramp(matrix,  9, palette::Id::STAR_WHITE);
    draw_fg_ramp(matrix, 11, palette::Id::STAR_BLUE);
    draw_fg_ramp(matrix, 13, palette::Id::STAR_AMBER);

    // ── Lower half: theme coverage (FR-15.8 / T.9) ──────────────────────
    // Reads theme:: every frame so a Core-0-driven theme cycle (in
    // main.cpp's loop()) is picked up at the next frame boundary
    // (FR-15.4 next-frame swap, no torn frames).
    matrix.drawFastHLine(0,  8, PANEL_WIDTH, theme::ink(theme::Ink::DIVIDER));
    matrix.drawFastHLine(0, 15, PANEL_WIDTH, theme::ink(theme::Ink::DIVIDER));

    // Theme HEADER: a 3-char tag drawn through the shared header helper
    // so brackets / BLOCK_BARS / NEON_OUTLINE all engage automatically.
    // The HEADER font is ~8 px tall; baseline at row 23 puts caps in
    // rows 16..22 with the halo bleeding to row 15/23 — overlapping the
    // divider lines deliberately so NEON_OUTLINE's glow reads.
    matrix.setFont(theme::font(theme::FontRole::HEADER));
    matrix.setTextSize(1);
    const char* tag = theme_tag_3(theme::current());
    gfx::draw_scene_header(matrix, tag, /*x=*/2, /*y=*/23,
                           theme::ink(theme::Ink::HEADER));
    // Restore the built-in font for subsequent setCursor + print calls
    // (draw_scene_header sets it back to nullptr internally, but be
    // explicit so future edits don't surprise themselves).
    matrix.setFont(nullptr);

    // CURSOR_BLOCK demo — a 2×4 block in ACCENT_MAGENTA after the
    // header tag, blinking at 1 Hz. Only renders when the active
    // theme declares CURSOR_BLOCK (Nostromo).
    if (theme::has(theme::Hint::CURSOR_BLOCK) &&
        ((now_ms / 500u) & 1u)) {
      matrix.fillRect(PANEL_WIDTH - 5, 18, 2, 5,
                      theme::ink(theme::Ink::ACCENT_MAGENTA));
    }

    // Ink swatch strip: 25 inks × 2 px wide × 2 px tall = 50 cols.
    // Order matches the Ink enum so a regression in any role is
    // visually adjacent to the one above/below it in the table.
    static_assert(static_cast<int>(theme::Ink::COUNT) <= 32,
                  "ink swatch row only sized for ≤32 inks");
    for (int i = 0; i < static_cast<int>(theme::Ink::COUNT); ++i) {
      const uint16_t c = theme::ink(static_cast<theme::Ink>(i));
      const int x = i * 2;
      matrix.drawPixel(x,     24, c);
      matrix.drawPixel(x + 1, 24, c);
      matrix.drawPixel(x,     25, c);
      matrix.drawPixel(x + 1, 25, c);
    }

    // FPS + theme tag readout in BODY ink.
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);
    const int16_t by = 30;
    char line[16];
    snprintf(line, sizeof(line), "%lu",
             static_cast<unsigned long>(m_fps_display));
    matrix.setTextColor(theme::ink(theme::Ink::BODY));
    matrix.setCursor(1, by);
    matrix.print(line);

    // GIANT_DIGIT_GHOST mini-demo: draw an "8" in GHOST ink behind a
    // "1" in BODY ink so Apollo's ghost-behind-live-digit pattern is
    // visible without a giant-clock-sized layout. Other themes
    // declare GHOST as a near-black so the mini-demo correctly fades
    // out — that's the same passthrough behaviour as the real giant
    // clock under non-Apollo themes.
    matrix.setTextColor(theme::ink(theme::Ink::GHOST));
    matrix.setCursor(28, by);
    matrix.print("8");
    matrix.setTextColor(theme::ink(theme::Ink::BODY));
    matrix.setCursor(28, by);
    matrix.print("1");

    // Uptime "Tn" in VALUE ink so VALUE / LABEL distinct from BODY.
    const uint32_t uptime_s = (now_ms - m_init_ms) / 1000u;
    matrix.setTextColor(theme::ink(theme::Ink::LABEL));
    matrix.setCursor(36, by);
    matrix.print("T");
    snprintf(line, sizeof(line), "%lus",
             static_cast<unsigned long>(uptime_s));
    matrix.setTextColor(theme::ink(theme::Ink::VALUE));
    matrix.setCursor(40, by);
    matrix.print(line);

    // Tiny "moving witness" pixel in the bottom-right that hops 1 px
    // every frame. If FPS feels off but the number says fine, watch the
    // pixel — irregular hopping = jitter, smooth march = real frame loss.
    const uint8_t hop = static_cast<uint8_t>(m_frames_in_window & 0x07);
    matrix.drawPixel(PANEL_WIDTH - 1 - hop, 31, 0xF800);  // bright red
  }

private:
  static void draw_fg_ramp(Adafruit_Protomatter& matrix,
                           int y0, palette::Id pal) {
    for (int x = 0; x < PANEL_WIDTH; ++x) {
      const uint16_t c = palette::fg(pal, static_cast<uint8_t>(x));
      matrix.drawPixel(x, y0,     c);
      matrix.drawPixel(x, y0 + 1, c);
    }
  }

  // Compact 3-char tag for the active theme — fits the HEADER band
  // even under chunkier HEADER fonts (Press Start 2P is 8 px wide per
  // glyph, so 3 chars ≈ 24 px). Order MUST match theme::Id.
  static const char* theme_tag_3(theme::Id id) {
    switch (id) {
      case theme::Id::APOLLO_AMBER:   return "APO";
      case theme::Id::NOSTROMO_GREEN: return "NOS";
      case theme::Id::VECTREX_NEON:   return "VEC";
      case theme::Id::BLADE_RUNNER:   return "BLR";
      case theme::Id::LCARS_TOS:      return "LCR";
      default:                         return "???";
    }
  }

  bool     m_initialised      = false;
  uint32_t m_init_ms          = 0;
  uint32_t m_last_tick_ms     = 0;
  uint16_t m_palette_shift    = 0;

  // Sliding 1 s window FPS sampler. window_start advances every time
  // we close a window, fps_display holds the last completed sample.
  uint32_t m_window_start_ms  = 0;
  uint32_t m_frames_in_window = 0;
  uint32_t m_fps_display      = 0;
};

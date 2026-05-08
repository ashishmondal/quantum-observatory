// Per-theme animated background for the giant clock scene.
//
// Each retro-sci-fi theme (theme::Id) gets a distinct ambient
// animation behind the giant HH:MM readout. All routines write every
// pixel (fillScreen + draw on top) and never call matrix.show() — same
// contract as the other *Bg classes (see backgrounds.h).
//
// Smoothness rationale: the panel runs ~60 fps but ms-per-pixel motion
// (e.g. 156 ms/row) reads as a strobe — the eye samples 9 frames of
// the same pixel position then sees it teleport. Two cures used here:
//
//   1. Short fading trails behind every moving head ("phosphor
//      persistence"). Even when the head is integer-stepped, a 4–6
//      row gradient tail fills the gap so motion reads as continuous
//      glow rather than discrete jumps. (Inks are picked from the
//      theme's own dim ramp so the trail stays in-palette.)
//   2. Continuous-time math where it matters. VECTREX's perspective
//      grid uses a `1/z` projection with a floating phase so lines
//      slide a sub-pixel per frame, then respawn at the top when they
//      cross the bottom edge — no LUT snapping.
//
// Per-theme effect:
//   APOLLO_AMBER   — single amber dot scanning the panel like a CRT
//                    electron beam (left→right at 64 px / 125 ms,
//                    row by row, top→bottom, then wrap). Behind the
//                    dot, a 3-scanline (192-pixel) fade through the
//                    STAR_AMBER palette ramp eases the trail to
//                    black so the motion reads as continuous glow.
//   NOSTROMO_GREEN — every-other column hosts a "memory dump" drop
//                    with a 4-row gradient tail. Per-column step
//                    period staggers fast/slow drops; a faint floor
//                    flicker plays as each drop wraps.
//   VECTREX_NEON   — vanishing-point ray fan (fixed) + perspective
//                    grid lines that slide forward continuously, each
//                    line drawn at `vp_y + K/z` so spacing tightens
//                    near the horizon. Lines respawn smoothly at the
//                    top when their z crosses zero.
//   BLADE_RUNNER   — diagonal cyan/magenta rain (1:4) with a 3-pixel
//                    trail per drop; a 1-frame +2 px global x-shift
//                    every ~100 frames simulates a signal hiccup.
//   LCARS_TOS      — 8×4 grid of 6×6 cells; one random cell toggles
//                    every 500 ms between Ink::STATUS_DIM and black
//                    (Enterprise sub-processor activity panels —
//                    intentionally stepped, not interpolated).
//
// State cost: ~160 B (per-column tables for Nostromo dominate). All
// RNG is a local LCG seeded deterministically in init() so the boot
// frame is identical across resets.

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "color_palette.h"
#include "config.h"
#include "theme.h"

class ThemeClockBg {
public:
  void init() {
    m_rng = 0xA5F03C2Du;

    // Nostromo column tables. Step periods picked from a small set so
    // visually-distinct fast/slow drops coexist; phase staggers
    // initial positions so the first frame doesn't show a synchronized
    // line. Smaller step values than before (was 60..280 ms) push the
    // head along faster — the trail does the heavy lifting on
    // perceived smoothness so we can run heads at ~25..60 ms/row.
    static constexpr uint16_t kStepChoices[4] = { 25, 40, 65, 100 };
    for (int i = 0; i < PANEL_WIDTH; ++i) {
      m_col_step_ms[i] = kStepChoices[next_rand() & 0x3];
      m_col_phase[i]   = static_cast<uint8_t>((next_rand() >> 8) & 0x3F);
    }

    // Blade Runner rain.
    for (int i = 0; i < kRain; ++i) br_respawn(i, /*initial=*/true);
    m_br_frame_count = 0;

    // LCARS: start sparse (~25% density via AND of two randoms).
    m_lcars_cells       = next_rand() & next_rand();
    m_lcars_last_toggle = 0;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    matrix.fillScreen(0x0000);
    switch (theme::current()) {
      case theme::Id::APOLLO_AMBER:   render_apollo(matrix, now_ms);       break;
      case theme::Id::NOSTROMO_GREEN: render_nostromo(matrix, now_ms);     break;
      case theme::Id::VECTREX_NEON:   render_vectrex(matrix, now_ms);      break;
      case theme::Id::BLADE_RUNNER:   render_blade_runner(matrix, now_ms); break;
      case theme::Id::LCARS_TOS:      render_lcars(matrix, now_ms);        break;
      default: break;
    }
  }

private:
  // ── APOLLO_AMBER: CRT raster dot ─────────────────────────────────
  // A single amber dot scans the panel like a CRT electron beam:
  // left→right at 64 px / 125 ms (one row per eighth-second), then
  // wraps to the start of the next row, finally wrapping back to
  // (0,0) after the bottom row. Behind the dot trails a 3-scanline
  // (192-pixel) fade through palette::STAR_AMBER, peak at the head
  // and easing to black exactly as it leaves the third row behind.
  //
  // Rate math:
  //   step = 125 ms / 64 px        ≈ 1.9531 ms/px
  //   path = 32 rows × 64 px       = 2048 px
  //   pass period                  = 2048 × 1.9531 ms = 4 s
  //
  // We work in a single linear "scan position" 0..2048 driven from
  // now_ms, then convert each trail offset back to (x,y) via /% 64.
  // STAR_AMBER's FG ramp (0..63) is the natural fade — index 63 is
  // peak amber, 0 is black — so trail brightness is simply
  // 63 - (offset * 63) / 192. (1-pixel hysteresis at offset 192
  // would otherwise leave a faint last pixel from integer rounding.)
  void render_apollo(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    constexpr uint32_t kRowMs    = 125;                   // 64 px in 125 ms
    constexpr int      kPathLen  = PANEL_WIDTH * PANEL_HEIGHT;
    constexpr uint32_t kPeriodMs = kRowMs * PANEL_HEIGHT; // full screen
    constexpr int      kTrail    = PANEL_WIDTH * 3;       // 3 scan lines

    // Sub-pixel resolution on the head: scale time so 1 unit = 1 px
    // step (kPathLen units per kPeriodMs). Avoids the 7.8 ms strobe.
    const uint32_t t       = now_ms % kPeriodMs;
    const int      head_lp = static_cast<int>(
        (static_cast<uint64_t>(t) * kPathLen) / kPeriodMs);

    // Draw the trail tail-first so the head overpaints any rounding
    // collision at offset 0 with full brightness.
    for (int off = kTrail; off >= 0; --off) {
      // Linear position behind the head, wrapping past frame start.
      int lp = head_lp - off;
      if (lp < 0) lp += kPathLen;
      const int x = lp % PANEL_WIDTH;
      const int y = lp / PANEL_WIDTH;
      // Palette FG ramp index 0..63 — linear fade from 63 at the
      // head to 0 at the third row behind. Skip br=0 (= black).
      const uint8_t br = static_cast<uint8_t>(
          63 - (static_cast<int>(off) * 63) / kTrail);
      if (br == 0) continue;
      matrix.drawPixel(x, y, palette::fg(palette::Id::STAR_AMBER, br));
    }
  }

  // ── NOSTROMO_GREEN: falling memory-dump columns ──────────────────
  // Every-other column hosts an independent drop. Head ink is the
  // bright STATUS_OK phosphor; below it a 4-row gradient (DIVIDER →
  // sparse DIVIDER) trails behind. The CYCLE adds a few rows of
  // off-panel dead time per column so the field doesn't read as a
  // solid wall of green. The bottom row briefly flickers as the head
  // wraps — kept from the previous version, it sells the spec's
  // "block hits the bottom" beat.
  void render_nostromo(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    const uint16_t head = theme::ink(theme::Ink::STATUS_OK);
    const uint16_t tail = theme::ink(theme::Ink::DIVIDER);
    constexpr int CYCLE = PANEL_HEIGHT + 10;

    for (int x = 1; x < PANEL_WIDTH; x += 2) {
      const uint16_t step  = m_col_step_ms[x];
      const uint8_t  phase = m_col_phase[x];
      const int      pos   = static_cast<int>(((now_ms / step) + phase) % CYCLE);

      // 4-row gradient trail. Top two rows full, bottom two sparse —
      // same dither trick as Apollo to fake a sub-RGB565 dim level.
      for (int dy = 1; dy <= 4; ++dy) {
        const int y = pos - dy;
        if (y < 0 || y >= PANEL_HEIGHT) continue;
        if (dy <= 2) {
          matrix.drawPixel(x, y, tail);
        } else {
          // Use the fast phase to dither across frames so the dim
          // section shimmers slightly — reads as phosphor decay.
          if (((now_ms >> 5) + dy + x) & 1) {
            matrix.drawPixel(x, y, tail);
          }
        }
      }

      if (pos >= 0 && pos < PANEL_HEIGHT) {
        const bool floor_flicker =
            (pos == PANEL_HEIGHT - 1) && ((now_ms / 80u) & 1u);
        if (!floor_flicker) matrix.drawPixel(x, pos, head);
      }
    }
  }

  // ── VECTREX_NEON: continuous perspective grid ────────────────────
  // Fixed converging-ray fan + grid lines that drift forward in
  // continuous time. Lines are positioned with a 1/z projection
  // around a vanishing point at (32, -10). Each line owns a slot
  // n ∈ [0..N); its depth is `z = (n + phase) * step` where phase
  // grows linearly with millis. As phase wraps, slot 0 effectively
  // respawns at the top (largest z, smallest screen y delta) while
  // the slot whose z reached the camera plane has already left the
  // bottom of the panel — no snapping, no LUT.
  //
  // K (the focal-length-like constant) and the step are tuned by
  // eye so the topmost visible line lands ~y=2 and the bottom one
  // exits past y=PANEL_HEIGHT smoothly.
  void render_vectrex(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    const uint16_t ray_ink  = theme::ink(theme::Ink::DIVIDER);   // dim cyan
    const uint16_t line_ink = theme::ink(theme::Ink::CHROME);    // cyan
    const uint16_t glow_ink = theme::ink(theme::Ink::HEADER_GLOW);

    // ── Rays: 9 lines fanning from the vanishing point through
    // x positions across (and past) the bottom edge. Hand-picked
    // so the spacing reads as roughly even-angular at the bottom.
    constexpr int16_t VPX = 32, VPY = -10;
    static constexpr int16_t kBottomX[] = {
        -40, -16, 0, 14, 24, 32, 40, 50, 64, 80, 104
    };
    for (size_t i = 0; i < sizeof(kBottomX) / sizeof(kBottomX[0]); ++i) {
      matrix.drawLine(VPX, VPY, kBottomX[i], PANEL_HEIGHT, ray_ink);
    }

    // ── Perspective grid. Slot phase advances continuously; one
    // full "respawn cycle" every kPeriodMs.
    constexpr float kK         = 220.0f;   // focal constant
    constexpr float kStep      = 1.0f;     // depth between slots
    constexpr int   kSlots     = 7;
    constexpr float kZMin      = 5.5f;     // skip slots that would land off the top
    constexpr uint32_t kPeriodMs = 1400;
    const float phase = static_cast<float>(now_ms % kPeriodMs) /
                        static_cast<float>(kPeriodMs);

    for (int n = 0; n < kSlots; ++n) {
      const float z = (static_cast<float>(n) + (1.0f - phase)) * kStep + kZMin;
      const int y = VPY + static_cast<int>(kK / z);
      if (y < 0 || y >= PANEL_HEIGHT) continue;
      // Closer rows (smaller n => larger y, bigger on screen) get
      // a glow row above for the neon thickening effect.
      matrix.drawFastHLine(0, y, PANEL_WIDTH, line_ink);
      if (n <= 2) {
        const int yg = y - 1;
        if (yg >= 0 && yg < PANEL_HEIGHT) {
          matrix.drawFastHLine(0, yg, PANEL_WIDTH, glow_ink);
        }
      }
    }
  }

  // ── BLADE_RUNNER: diagonal rain + glitch ─────────────────────────
  // Drops walk +1 px/frame in both axes (45°). Each drop now leaves a
  // 3-pixel trail (head + 2 dimmer steps up-left) so its motion
  // reads as a continuous streak across a frame even at panel-refresh
  // boundaries. A 1-frame +2 px global x-offset every 100 frames
  // simulates a signal hiccup. Color split is 1 magenta : 4 cyan.
  void render_blade_runner(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    (void)now_ms;
    ++m_br_frame_count;
    const int16_t  dx       = ((m_br_frame_count % 100u) == 0) ? 2 : 0;
    const uint16_t cyan     = theme::ink(theme::Ink::CHROME);
    const uint16_t magenta  = theme::ink(theme::Ink::ACCENT_MAGENTA);
    const uint16_t cyan_dim = theme::ink(theme::Ink::DIVIDER);
    const uint16_t mag_dim  = theme::ink(theme::Ink::HEADER_GLOW);

    for (int i = 0; i < kRain; ++i) {
      m_br_x[i] = static_cast<int8_t>(m_br_x[i] + 1);
      m_br_y[i] = static_cast<int8_t>(m_br_y[i] + 1);
      if (m_br_x[i] >= static_cast<int8_t>(PANEL_WIDTH) ||
          m_br_y[i] >= static_cast<int8_t>(PANEL_HEIGHT)) {
        br_respawn(i, /*initial=*/false);
      }
      const int16_t  px      = m_br_x[i] + dx;
      const int16_t  py      = m_br_y[i];
      const bool     is_mag  = m_br_color[i] != 0u;
      const uint16_t bright  = is_mag ? magenta : cyan;
      const uint16_t dim     = is_mag ? mag_dim : cyan_dim;

      // Trail: 2 dim steps up-left of the head, then the head on top.
      for (int s = 2; s >= 1; --s) {
        const int16_t tx = px - s;
        const int16_t ty = py - s;
        if (tx >= 0 && tx < PANEL_WIDTH && ty >= 0 && ty < PANEL_HEIGHT) {
          matrix.drawPixel(tx, ty, dim);
        }
      }
      if (px >= 0 && px < PANEL_WIDTH && py >= 0 && py < PANEL_HEIGHT) {
        matrix.drawPixel(px, py, bright);
      }
    }
  }

  // ── LCARS_TOS: stepped block grid ────────────────────────────────
  // 8×4 cells of 8×8 footprint; we paint a 6×6 inner rect leaving a
  // 1 px gap on every side so adjacent cells read as discrete blocks.
  // One random cell toggles every 500 ms — random walk in density,
  // matching the "sub-processor activity" Enterprise bridge feel.
  // Stepped on purpose: LCARS panels famously DO NOT smooth-animate.
  void render_lcars(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    if ((now_ms - m_lcars_last_toggle) >= 500u) {
      m_lcars_last_toggle = now_ms;
      const uint32_t pick = next_rand() % 32u;
      m_lcars_cells ^= (1u << pick);
    }

    const uint16_t ink = theme::ink(theme::Ink::STATUS_DIM);
    constexpr int CELL = 8;
    constexpr int COLS = PANEL_WIDTH  / CELL;   // 8
    constexpr int ROWS = PANEL_HEIGHT / CELL;   // 4
    for (int i = 0; i < COLS * ROWS; ++i) {
      if (!(m_lcars_cells & (1u << i))) continue;
      const int cx = (i % COLS) * CELL;
      const int cy = (i / COLS) * CELL;
      matrix.fillRect(cx + 1, cy + 1, CELL - 2, CELL - 2, ink);
    }
  }

  // ── Helpers / state ──────────────────────────────────────────────
  uint32_t next_rand() {
    m_rng = m_rng * 1664525u + 1013904223u;
    return m_rng;
  }

  void br_respawn(int i, bool initial) {
    // Spawn off the top-left edge so the diagonal walk slides drops
    // onto the panel from the corner band.
    if (initial || (next_rand() & 1u)) {
      m_br_x[i] = -static_cast<int8_t>((next_rand() >> 8) % PANEL_HEIGHT);
      m_br_y[i] = -static_cast<int8_t>((next_rand() >> 16) % 8u);
    } else {
      m_br_x[i] =  static_cast<int8_t>((next_rand() >> 8) % PANEL_WIDTH);
      m_br_y[i] = -static_cast<int8_t>(((next_rand() >> 16) % PANEL_HEIGHT) + 1u);
    }
    // 1 magenta : 4 cyan ratio.
    m_br_color[i] = static_cast<uint8_t>(((next_rand() >> 24) % 5u) == 0u ? 1u : 0u);
  }

  uint32_t m_rng = 0;

  // Nostromo
  uint16_t m_col_step_ms[PANEL_WIDTH] = {};
  uint8_t  m_col_phase[PANEL_WIDTH]   = {};

  // Blade Runner rain — sparse enough to read as weather, dense
  // enough to keep something always crossing the panel.
  static constexpr int kRain = 18;
  int8_t   m_br_x[kRain]      = {};
  int8_t   m_br_y[kRain]      = {};
  uint8_t  m_br_color[kRain]  = {};
  uint32_t m_br_frame_count   = 0;

  // LCARS
  uint32_t m_lcars_cells       = 0;   // bit i = cell (i % 8, i / 8)
  uint32_t m_lcars_last_toggle = 0;
};

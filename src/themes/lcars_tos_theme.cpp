// LCARS_TOS — see include/themes/lcars_tos_theme.h.

#include "themes/lcars_tos_theme.h"

#include <Adafruit_GFX.h>
#include <Adafruit_Protomatter.h>
#include <Fonts/Org_01.h>
#include <Fonts/Tiny3x3a2pt7b.h>

#include "buzzer.h"
#include "config.h"
#include "fonts/digital_7__mono_14pt7b.h"
#include "fonts/pixel_operator_8pt7b.h"

namespace theme {

namespace {

// LCARS_TOS — Star Trek LCARS-precursor (THEME.md §2.5). Colored
// solid blocks instead of brackets, blocky orange/yellow/red sans
// labels. Brackets are intentionally empty — the BLOCK_BARS primitive
// (gfx_text.h) draws colored block bars in their place.
constexpr uint16_t kInks[static_cast<int>(Ink::COUNT)] = {
  /*CHROME         */ 0xFD20,
  /*CHROME_HALO    */ 0x0000,
  /*HEADER         */ 0xFD20,
  /*HEADER_HALO    */ 0x0000,
  /*HEADER_GLOW    */ 0x0000,
  /*HEADER_DIM     */ 0x4080,
  /*BODY           */ 0xFD20,
  /*BODY_HALO      */ 0x0000,
  /*BODY_GLOW      */ 0x0000,
  /*ACCENT         */ 0xFFE0,
  /*ACCENT_MAGENTA */ 0xF81F,
  /*ALERT          */ 0xF800,
  /*GHOST          */ 0x0820,
  /*DIVIDER        */ 0xFD20,
  /*GIANT_DIGITS   */ 0xFD20,
  /*STATUS_OK      */ 0xFFE0,
  /*STATUS_OK_DIM  */ 0x4200,
  /*STATUS_WARN    */ 0xFD20,
  /*STATUS_WARN_DIM*/ 0x4080,
  /*STATUS_INFO    */ 0xFFE0,
  /*STATUS_STALE   */ 0x4080,
  /*STATUS_DIM     */ 0x4080,
  /*LABEL          */ 0x4080,
  /*VALUE          */ 0xFD20,
  /*SAFETY         */ 0xF800,   // LCARS red-alert IS safety
};

constexpr const GFXfont* kFonts[static_cast<int>(FontRole::COUNT)] = {
  /*MICRO */ &Tiny3x3a2pt7b,
  /*BODY  */ &Org_01,                  // 5×6 sans — LCARS sans label feel
  /*HEADER*/ &PixelOperator88pt7b,
  /*CLOCK */ &digital_7__mono_14pt7b,
};

constexpr uint32_t kHintMask =
    (1u << static_cast<uint32_t>(Hint::FRAME_BORDER)) |
    (1u << static_cast<uint32_t>(Hint::BLOCK_BARS));

constexpr BgRamp kBgRamp = {
  {0x000000, 0xCC0000, 0xFF9933, 0xFFE080},
};

}  // namespace

LcarsTosTheme::LcarsTosTheme()
    : Theme(Id::LCARS_TOS,
            "lcars_tos",
            "LCARS TOS",
            kInks,
            kFonts,
            kHintMask,
            "", "",                  // BLOCK_BARS replaces bracket glyphs
            &kBgRamp) {}

uint32_t LcarsTosTheme::next_rand() {
  m_rng = m_rng * 1664525u + 1013904223u;
  return m_rng;
}

void LcarsTosTheme::init_clock_bg() {
  m_rng = 0xA5F03C2Du;
  // Start sparse (~25% density via AND of two randoms).
  m_cells       = next_rand() & next_rand();
  m_last_toggle = 0;
  // Seed a color for every cell so initially-on cells aren't all the
  // same hue. Index 0..4 — see kCellPalette below.
  for (int i = 0; i < 32; ++i) {
    m_color_idx[i] = static_cast<uint8_t>(next_rand() % 5u);
  }
}

// User-supplied LCARS swatch — a 5-stop ramp from bright orange down
// through red-orange / burnt orange / rust to brown. All same-hue
// family (~15–30°) but stepped in Value, so blocks read as distinct
// luminance tiers rather than a flat orange wash.
//
// Gamma-corrected (γ=2.2) before packing: the HUB75 matrix drives
// pixels with linear PWM, so writing the raw sRGB swatch values
// would look washed out (mid-grays end up far too bright). For each
// channel we compute  out = (in/255)^2.2 * 255  then quantize to
// 5/6/5. Reference sRGB → linear8 → RGB565:
//   #FF9933 → (255, 81,  8) → 0xFA81  bright orange
//   #E55A22 → (200, 25,  3) → 0xC0C0  red-orange
//   #B84020 → (125, 13,  3) → 0x7860  burnt orange
//   #802810 → ( 56,  4,  1) → 0x3820  dark rust
//   #401810 → ( 13,  1,  1) → 0x1000  brown
constexpr uint16_t kCellPalette[5] = {
  0xFA81, 0xC0C0, 0x7860, 0x3820, 0x1000,
};

// 8×4 cells of 8×8 footprint; we paint a 6×6 inner rect leaving a
// 1 px gap on every side so adjacent cells read as discrete blocks.
// One random cell toggles every 500 ms — random walk in density,
// matching the "sub-processor activity" Enterprise bridge feel.
// Stepped on purpose: LCARS panels famously DO NOT smooth-animate.
// On every off→on toggle the cell gets a freshly-rolled palette
// color so the grid keeps shifting hue, not just density.
void LcarsTosTheme::render_clock_bg(Adafruit_Protomatter& matrix,
                                    uint32_t now_ms) {
  matrix.fillScreen(0x0000);

  if ((now_ms - m_last_toggle) >= 500u) {
    m_last_toggle = now_ms;
    const uint32_t pick = next_rand() % 32u;
    const bool turning_on = !(m_cells & (1u << pick));
    m_cells ^= (1u << pick);
    if (turning_on) {
      m_color_idx[pick] = static_cast<uint8_t>(next_rand() % 5u);
    }
  }

  constexpr int CELL = 8;
  constexpr int COLS = PANEL_WIDTH  / CELL;   // 8
  constexpr int ROWS = PANEL_HEIGHT / CELL;   // 4
  for (int i = 0; i < COLS * ROWS; ++i) {
    if (!(m_cells & (1u << i))) continue;
    const int cx = (i % COLS) * CELL;
    const int cy = (i / COLS) * CELL;
    matrix.fillRect(cx + 1, cy + 1, CELL - 2, CELL - 2,
                    kCellPalette[m_color_idx[i]]);
  }
}

// ── FR-10.7 signature melody ────────────────────────────────────
// "Courage Fanfare" — Star Trek opening climb. Three short
// stepping notes into a held top, total 850 ms.
namespace {
constexpr buzzer::Note kMelody[] = {
  { 8372, 150 },
  {11175, 150 },
  { 9956, 150 },
  {14080, 400 },
};
}  // namespace

Melody LcarsTosTheme::melody() const {
  return { kMelody, sizeof(kMelody) / sizeof(kMelody[0]) };
}

LcarsTosTheme g_lcars_tos_theme;

}  // namespace theme

// BLADE_RUNNER — see include/themes/blade_runner_theme.h.

#include "themes/blade_runner_theme.h"

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

// BLADE_RUNNER — late-80s neo-noir HUD (THEME.md §2.4). Saturated
// cyan + orange on deep blue, framed panel border, neon halos.
// Halo orange dropped to ~25% V from 0xFD20 so it reads as warm glow
// shadow under cyan text.
constexpr uint16_t kInks[static_cast<int>(Ink::COUNT)] = {
  /*CHROME         */ 0x07FF,
  /*CHROME_HALO    */ 0x0000,
  /*HEADER         */ 0x07FF,
  /*HEADER_HALO    */ 0x4140,
  /*HEADER_GLOW    */ 0x4140,
  /*HEADER_DIM     */ 0x0210,
  /*BODY           */ 0x07FF,
  /*BODY_HALO      */ 0x0000,
  /*BODY_GLOW      */ 0x4140,
  /*ACCENT         */ 0xFD20,
  /*ACCENT_MAGENTA */ 0xF81F,
  /*ALERT          */ 0xF800,
  /*GHOST          */ 0x0041,
  /*DIVIDER        */ 0x07FF,
  /*GIANT_DIGITS   */ 0x07FF,
  /*STATUS_OK      */ 0x07FF,
  /*STATUS_OK_DIM  */ 0x0210,
  /*STATUS_WARN    */ 0xFD20,
  /*STATUS_WARN_DIM*/ 0x4080,
  /*STATUS_INFO    */ 0x07FF,
  /*STATUS_STALE   */ 0x4080,
  /*STATUS_DIM     */ 0x0210,
  /*LABEL          */ 0x4080,
  /*VALUE          */ 0x07FF,
  /*SAFETY         */ 0x4000,
};

constexpr const GFXfont* kFonts[static_cast<int>(FontRole::COUNT)] = {
  /*MICRO */ &Tiny3x3a2pt7b,
  /*BODY  */ &Org_01,                  // 5×6 sans w/ true lowercase — HUD feel
  /*HEADER*/ &PixelOperator88pt7b,
  /*CLOCK */ &digital_7__mono_14pt7b,
};

constexpr uint32_t kHintMask =
    (1u << static_cast<uint32_t>(Hint::FRAME_BORDER)) |
    (1u << static_cast<uint32_t>(Hint::NEON_OUTLINE));

constexpr BgRamp kBgRamp = {
  {0x000000, 0xFF7000, 0x00E0FF, 0xE0F8FF},
};

}  // namespace

BladeRunnerTheme::BladeRunnerTheme()
    : Theme(Id::BLADE_RUNNER,
            "blade_runner",
            "BLADE RUNNER",
            kInks,
            kFonts,
            kHintMask,
            ":", ":",
            &kBgRamp) {}

uint32_t BladeRunnerTheme::next_rand() {
  m_rng = m_rng * 1664525u + 1013904223u;
  return m_rng;
}

void BladeRunnerTheme::respawn(int i, bool initial) {
  // Drops travel +1/+1 (down-right) so they enter from the TOP edge
  // (y=-1, x ∈ [0,W)) or the LEFT edge (x=-1, y ∈ [0,H)). Picking an
  // entry point uniformly along the combined edge of length W+H gives
  // every visible diagonal d=y−x ∈ [−(W−1), H−1] equal probability —
  // without that, the old left/top 50-50 split over-sampled the
  // mid-left diagonals and starved the upper-right.
  //
  // A random back-delay along the diagonal staggers arrival times so
  // drops don't pile up at the edge after respawn.
  const uint32_t r1   = next_rand();
  const uint32_t edge = (r1 >> 8) % (PANEL_WIDTH + PANEL_HEIGHT);
  int16_t ex, ey;
  if (edge < PANEL_WIDTH) {
    ex = static_cast<int16_t>(edge);
    ey = -1;
  } else {
    ex = -1;
    ey = static_cast<int16_t>(edge - PANEL_WIDTH);
  }
  // Stagger: walk the spawn back along the −1/−1 diagonal by a random
  // amount. On the very first init, spread fully across the panel
  // span so the field starts populated; afterwards a shorter delay
  // keeps the rain dense.
  const uint32_t span  = initial ? (PANEL_WIDTH + PANEL_HEIGHT)
                                 : PANEL_HEIGHT;
  const uint32_t delay = ((r1 >> 20) ^ (next_rand() >> 8)) % span;
  m_x[i] = static_cast<int8_t>(ex - static_cast<int16_t>(delay));
  m_y[i] = static_cast<int8_t>(ey - static_cast<int16_t>(delay));
  // 1 magenta : 4 cyan ratio.
  m_color[i] = static_cast<uint8_t>(((next_rand() >> 24) % 5u) == 0u ? 1u : 0u);
}

void BladeRunnerTheme::init_clock_bg() {
  m_rng = 0xA5F03C2Du;
  for (int i = 0; i < kRain; ++i) respawn(i, /*initial=*/true);
  m_frame_count = 0;
}

// Drops walk +1 px/frame in both axes (45°). Each drop leaves a
// 3-pixel trail (head + 2 dimmer steps up-left). A 1-frame +2 px
// global x-offset every 100 frames simulates a signal hiccup.
void BladeRunnerTheme::render_clock_bg(Adafruit_Protomatter& matrix,
                                       uint32_t now_ms) {
  matrix.fillScreen(0x0000);
  (void)now_ms;
  ++m_frame_count;
  const int16_t  dx       = ((m_frame_count % 100u) == 0) ? 2 : 0;
  const uint16_t cyan     = ink(Ink::CHROME);
  const uint16_t magenta  = ink(Ink::ACCENT_MAGENTA);
  const uint16_t cyan_dim = ink(Ink::DIVIDER);
  const uint16_t mag_dim  = ink(Ink::HEADER_GLOW);

  for (int i = 0; i < kRain; ++i) {
    m_x[i] = static_cast<int8_t>(m_x[i] + 1);
    m_y[i] = static_cast<int8_t>(m_y[i] + 1);
    if (m_x[i] >= static_cast<int8_t>(PANEL_WIDTH) ||
        m_y[i] >= static_cast<int8_t>(PANEL_HEIGHT)) {
      respawn(i, /*initial=*/false);
    }
    const int16_t  px      = m_x[i] + dx;
    const int16_t  py      = m_y[i];
    const bool     is_mag  = m_color[i] != 0u;
    const uint16_t bright  = is_mag ? magenta : cyan;
    const uint16_t dim     = is_mag ? mag_dim : cyan_dim;

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

// ── FR-10.7 signature melody ────────────────────────────────────
// "Vangelis CS-80 Sweep" — three-step falling swell, total 800 ms.
namespace {
constexpr buzzer::Note kMelody[] = {
  // "CS-80 descent" — noir minor-key fall D5 → C5 → G#4. The drop to
  // G#4 is a tritone below D5 (the Vangelis "End Titles" mood interval)
  // and the long held tail leaves it unresolved — reads as the
  // signature replicant-noir cue. Total = 280+280+700 = 1260 ms.
  { 587, 280 },   // D5
  { 523, 280 },   // C5
  { 415, 700 },   // G#4 — held, unresolved
};
}  // namespace

Melody BladeRunnerTheme::melody() const {
  return { kMelody, sizeof(kMelody) / sizeof(kMelody[0]) };
}

BladeRunnerTheme g_blade_runner_theme;

}  // namespace theme

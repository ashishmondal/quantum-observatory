// VECTREX_NEON — see include/themes/vectrex_neon_theme.h.

#include "themes/vectrex_neon_theme.h"

#include <cstdlib>            // std::abs

#include <Adafruit_GFX.h>
#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>
#include <Fonts/Tiny3x3a2pt7b.h>

#include "buzzer.h"
#include "config.h"
#include "fonts/digital_7__mono_14pt7b.h"
#include "fonts/pixel_operator_8pt7b.h"

namespace theme {

namespace {

// VECTREX_NEON — vector arcade (THEME.md §2.3). Identity rides on the
// cyan + magenta complementary pair plus the NEON_OUTLINE hint.
// Halo values are dropped to ~6% V of the foreground so they read as
// a faint glow shadow under the cyan text rather than fighting it for
// attention. (Earlier ~25% V halo at 0x3807 visibly clashed with the
// CHROME cyan — same hue-distance, too close in luminance, the eye
// read it as a second outline rather than a halo.)
constexpr uint16_t kInks[static_cast<int>(Ink::COUNT)] = {
  /*CHROME         */ 0x07FF,   // cyan (H=180°, V=100%)
  /*CHROME_HALO    */ 0x1002,   // ~6% V magenta halo (R=2, B=2)
  /*HEADER         */ 0x07FF,
  /*HEADER_HALO    */ 0x1002,
  /*HEADER_GLOW    */ 0x1002,
  /*HEADER_DIM     */ 0x0210,
  /*BODY           */ 0x07FF,
  /*BODY_HALO      */ 0x1002,
  /*BODY_GLOW      */ 0x1002,
  /*ACCENT         */ 0xFFFF,
  /*ACCENT_MAGENTA */ 0xF81F,
  /*ALERT          */ 0xF800,
  /*GHOST          */ 0x0041,
  /*DIVIDER        */ 0x0210,
  /*GIANT_DIGITS   */ 0xF81F,   // magenta — complement of cyan grid bg
  /*STATUS_OK      */ 0x07FF,
  /*STATUS_OK_DIM  */ 0x0210,
  /*STATUS_WARN    */ 0xF81F,
  /*STATUS_WARN_DIM*/ 0x4008,
  /*STATUS_INFO    */ 0x07FF,
  /*STATUS_STALE   */ 0x4008,
  /*STATUS_DIM     */ 0x0210,
  /*LABEL          */ 0x4008,
  /*VALUE          */ 0xFFFF,
  /*SAFETY         */ 0x4000,
};

constexpr const GFXfont* kFonts[static_cast<int>(FontRole::COUNT)] = {
  /*MICRO */ &Tiny3x3a2pt7b,
  /*BODY  */ &Picopixel,
  /*HEADER*/ &PixelOperator88pt7b,
  /*CLOCK */ &digital_7__mono_14pt7b,
};

constexpr uint32_t kHintMask =
    1u << static_cast<uint32_t>(Hint::NEON_OUTLINE);

constexpr BgRamp kBgRamp = {
  {0x000000, 0xFF00FF, 0x00FFFF, 0xFFFFFF},
};

}  // namespace

VectrexNeonTheme::VectrexNeonTheme()
    : Theme(Id::VECTREX_NEON,
            "vectrex_neon",
            "VECTREX NEON",
            kInks,
            kFonts,
            kHintMask,
            "<", ">",
            &kBgRamp) {}

// ── Dual perspective grid (floor + ceiling) ─────────────────────
// Camera flies along the axis; floor grid recedes below a horizon
// bisecting the panel, ceiling grid mirrors above.
//
// Density math for 64×32:
//   PANEL_HEIGHT/2 = 16 rows per half. We use 5 slots with strongly
//   increasing 1/z spacing so the eye reads it as depth, not a ladder.
//   Ray fan likewise: 7 rays per half — any more becomes a fill.
//
// Lines snap to integer rows (no sub-pixel anti-aliasing). At this
// resolution AA produced a 2-row "fat" line that read as motion blur;
// a single crisp 1-px line per scan reads cleaner. Forward-motion
// illusion comes from the 1/z spacing between slots, not from glide.
//
// Depth-fade gradient: every grid pixel is coloured by a small
// 8-stop cyan ramp (`kCyanRamp`) keyed by its distance from the
// horizon line. Pixels next to the horizon (= farthest from the
// camera) ride near the dim end of the ramp; pixels at the panel
// edges (= closest to the camera) ride at full brightness. This
// applies uniformly to scan-lines (one ramp index per line) and
// to the ray fan (per-pixel walk along each ray, sampled by row
// distance from the vanishing point).
namespace {

// 8-stop cyan ramp, dim → bright. RGB565 cyan = (0, G, B) with
// G ≤ 63 and B ≤ 31; we step G/2 = B so the hue stays pure cyan
// across the ramp. Stop 0 is "barely visible" (G=4) — anything
// dimmer reads as black on a 5-bit panel; stop 7 is the full
// CHROME ink (0x07FF).
constexpr uint8_t  kRampSteps                  = 8;
constexpr uint16_t kCyanRamp[kRampSteps] = {
  0x0082,  // G=4,  B=2   — far / horizon
  0x0124,  // G=9,  B=4
  0x01A6,  // G=13, B=6
  0x0248,  // G=18, B=8
  0x02CA,  // G=22, B=10
  0x036C,  // G=27, B=12
  0x03EE,  // G=31, B=14
  0x07FF,  // G=63, B=31  — near / panel edge (full CHROME)
};

// Map a 0..max distance to a ramp index 0..kRampSteps-1.
inline uint16_t cyan_at(int dist, int max_dist) {
  if (max_dist <= 0) return kCyanRamp[kRampSteps - 1];
  int idx = (dist * (kRampSteps - 1)) / max_dist;
  if (idx < 0) idx = 0;
  if (idx >= kRampSteps) idx = kRampSteps - 1;
  return kCyanRamp[idx];
}

// Walk a line from (x0,y0) to (x1,y1) and pick the ink for each
// pixel from `cyan_at(|y - vp_y|, max_dist)` so the segment fades
// out toward the vanishing-row. Bresenham; matches Adafruit_GFX's
// drawLine pixel order so the result reads identically modulo the
// per-pixel ink swap.
void draw_faded_ray(Adafruit_Protomatter& matrix,
                    int x0, int y0, int x1, int y1,
                    int vp_y, int max_dist) {
  int dx =  std::abs(x1 - x0);
  int dy = -std::abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  while (true) {
    if (x0 >= 0 && x0 < PANEL_WIDTH && y0 >= 0 && y0 < PANEL_HEIGHT) {
      const int dist = std::abs(y0 - vp_y);
      matrix.drawPixel(x0, y0, cyan_at(dist, max_dist));
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

}  // namespace

void VectrexNeonTheme::render_clock_bg(Adafruit_Protomatter& matrix,
                                       uint32_t now_ms) {
  matrix.fillScreen(0x0000);

  constexpr int     HORIZON = PANEL_HEIGHT / 2;     // 16
  constexpr int16_t VPX     = PANEL_WIDTH  / 2;     // 32
  constexpr int16_t VPY_F   = HORIZON;              // floor VP row
  constexpr int16_t VPY_C   = HORIZON - 1;          // ceiling VP row

  // Max distance for the cyan-ramp lookup. Both grids span up to
  // ~16 rows from their respective vanishing row to the panel edge,
  // so the ramp's full range is exercised exactly once across each
  // half. (max_dist also gates the scan-line fade below.)
  constexpr int kMaxDist = HORIZON;

  // No explicit horizon line — the grid lines already converge
  // toward the centre band and the gap between floor and ceiling
  // grids reads as the horizon on its own.

  // Horizon clear band — rows of black above + below the vanishing
  // line. Both the ray fan and the perspective scan-lines respect
  // this gap so the centre reads as "infinity" rather than a hard
  // crossing point. Matches the reference Vectrex screen where the
  // fan rays visibly stop short of the horizon, leaving a clean
  // black strip between the floor and ceiling planes.
  constexpr int kHorizonGap = 3;

  // Ray fan: 4 rays per half. Endpoints land near the panel edges
  // so the rays exit through the sides; the three previously-central
  // rays (ex=24, 32, 40) were removed because at horizon-band start
  // they collapsed into a tight 2-px-spacing cluster around the
  // vanishing point. Dropping them widens the centre while leaving
  // the outer/edge ray spacing unchanged. Each ray walks pixel-by-
  // pixel through draw_faded_ray() so its colour fades from full
  // CHROME at the panel edge to the dimmest ramp stop near the
  // horizon — same depth-cue as the scan-lines.
  static constexpr int16_t kEndX[] = { -8, 12, 52, 72 };
  // Floor rays span dy = 0..(PANEL_HEIGHT-1 - VPY_F) rows below VPY_F;
  // ceiling rays mirror that span above VPY_C. Interpolating x at
  // dy = kHorizonGap gives the on-panel start point.
  constexpr int kFloorSpan = PANEL_HEIGHT - 1 - VPY_F;     // 15
  constexpr int kCeilSpan  = VPY_C;                         // 15
  for (size_t i = 0; i < sizeof(kEndX) / sizeof(kEndX[0]); ++i) {
    const int16_t ex = kEndX[i];

    const int16_t fx_start = static_cast<int16_t>(
        VPX + (ex - VPX) * kHorizonGap / kFloorSpan);
    const int16_t fy_start = VPY_F + kHorizonGap;
    draw_faded_ray(matrix, fx_start, fy_start, ex, PANEL_HEIGHT - 1,
                   VPY_F, kMaxDist);

    const int16_t cx_start = static_cast<int16_t>(
        VPX + (ex - VPX) * kHorizonGap / kCeilSpan);
    const int16_t cy_start = VPY_C - kHorizonGap;
    draw_faded_ray(matrix, cx_start, cy_start, ex, 0, VPY_C, kMaxDist);
  }

  // Perspective scan-lines. Linear depth slots; phase advances
  // continuously and slot N wraps into slot N-1's depth on each
  // cycle, giving continuous motion without spawn pop. Each line's
  // ink is sampled from kCyanRamp by `dy` so the line nearest the
  // horizon paints in the dimmest stop and the line nearest the
  // panel edge paints in full CHROME — same gradient as the rays.
  constexpr float    kK          = 14.0f;
  constexpr float    kStep       = 1.2f;
  constexpr int      kSlots      = 5;
  constexpr float    kZMin       = 1.0f;
  constexpr uint32_t kPeriodMs   = 600;
  const float phase = static_cast<float>(now_ms % kPeriodMs) /
                      static_cast<float>(kPeriodMs);

  for (int n = 0; n < kSlots; ++n) {
    const float z  = (static_cast<float>(n) + (1.0f - phase)) * kStep + kZMin;
    const int   dy = static_cast<int>(kK / z);

    // Skip any scan-line that would land inside the horizon band.
    if (dy < kHorizonGap) continue;

    const uint16_t line_ink = cyan_at(dy, kMaxDist);

    const int y_floor = VPY_F + dy;
    if (y_floor >= 0 && y_floor < PANEL_HEIGHT) {
      matrix.drawFastHLine(0, y_floor, PANEL_WIDTH, line_ink);
    }

    const int y_ceil = VPY_C - dy;
    if (y_ceil >= 0 && y_ceil < PANEL_HEIGHT) {
      matrix.drawFastHLine(0, y_ceil, PANEL_WIDTH, line_ink);
    }
  }
}

// ── FR-10.7 signature melody ────────────────────────────────────
// "Arcade Attract" — minor-7th arpeggio shimmer. Three quick
// 60 ms grace notes ramping into a 120 ms held top, total 300 ms.
namespace {
constexpr buzzer::Note kMelody[] = {
  { 8372,  60 },
  { 9956,  60 },
  {12543,  60 },
  {14917, 120 },
};
}  // namespace

Melody VectrexNeonTheme::melody() const {
  return { kMelody, sizeof(kMelody) / sizeof(kMelody[0]) };
}

VectrexNeonTheme g_vectrex_neon_theme;

}  // namespace theme

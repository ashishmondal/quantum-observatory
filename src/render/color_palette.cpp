// See color_palette.h. Built at boot; read-only thereafter.

#include "color_palette.h"

#include <stdint.h>

namespace palette {

namespace {

// One color stop along a palette region. `pos` is the LOCAL index within
// that region (0..BG_LEN-1 for BG, 0..FG_LEN-1 for FG). Stops MUST be
// sorted ascending by `pos` and MUST cover the full region (first stop at
// 0, last at region_len - 1). For BG regions intended to cycle smoothly,
// the last stop should match the first (or be near it) so entry 191 flows
// back into entry 0 without a visible seam.
struct Stop {
  uint8_t pos;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

// --- Palette region definitions -----------------------------------------

// FG ramp (64 entries) — neutral white with a hint of warmth at the dim end.
constexpr Stop kStarWhiteFg[] = {
  {  0,   0,   0,   0},
  { 10,  18,  14,  10},
  { 30, 110, 105, 100},
  { 63, 255, 250, 245},
};

// FG ramp — Vega/Sirius look.
constexpr Stop kStarBlueFg[] = {
  {  0,   0,   0,   0},
  { 12,   8,  18,  60},
  { 35,  60, 130, 210},
  { 63, 200, 230, 255},
};

// FG ramp — Antares/Aldebaran blackbody-style ramp.
constexpr Stop kStarAmberFg[] = {
  {  0,   0,   0,   0},
  { 15,  60,  10,   0},
  { 40, 200,  90,  20},
  { 63, 255, 210, 140},
};

// BG ramp (192 entries) — closed loop through nebula colors. Last stop
// matches the first so cycling produces no visible seam at index 191->0.
// Walks deep violet -> magenta -> warm pink -> teal -> deep blue -> back.
constexpr Stop kNebulaCloudsBg[] = {
  {  0,  20,   0,  40},   // deep violet
  { 30,  60,  10,  90},   // violet
  { 60, 140,  20, 110},   // magenta
  { 90, 180,  60,  90},   // warm pink
  {120,  40, 110, 110},   // teal
  {150,  10,  40, 110},   // deep blue
  {180,  20,  10,  60},   // back toward violet
  {191,  20,   0,  40},   // close the loop
};

// Night-sky background fill. Single dark navy, gently brighter at the
// dim end of the ramp than the bright end so callers can pick how much
// presence the wash has by varying the fg() index. (Stars draw on top
// at much higher br6 values, so the wash never competes with them.)
constexpr Stop kNightSkyFg[] = {
  {  0,   0,   0,   0},
  { 20,   1,   2,   8},   // barely-there glow at low end
  { 63,   3,   5,  16},   // dim navy at full
};

// --- Palette table layout -----------------------------------------------

struct Region {
  const Stop* stops;
  uint8_t     count;   // 0 means region is unused (fill with black)
};

struct Def {
  Region bg;
  Region fg;
};

// Order MUST match Id enum.
constexpr Def kDefs[static_cast<int>(Id::COUNT)] = {
  // STAR_WHITE — FG only
  { {nullptr, 0}, {kStarWhiteFg, sizeof(kStarWhiteFg) / sizeof(Stop)} },
  // STAR_BLUE  — FG only
  { {nullptr, 0}, {kStarBlueFg,  sizeof(kStarBlueFg)  / sizeof(Stop)} },
  // STAR_AMBER — FG only
  { {nullptr, 0}, {kStarAmberFg, sizeof(kStarAmberFg) / sizeof(Stop)} },
  // NEBULA_CLOUDS — BG only
  { {kNebulaCloudsBg, sizeof(kNebulaCloudsBg) / sizeof(Stop)}, {nullptr, 0} },
  // NIGHT_SKY — FG only (used for the static starfield background wash)
  { {nullptr, 0}, {kNightSkyFg, sizeof(kNightSkyFg) / sizeof(Stop)} },
};

// 256 entries × 2 B × N palettes. With 4 palettes that's 2 KB SRAM —
// well within NFR-2.1.
uint16_t g_tables[static_cast<int>(Id::COUNT)][256];

inline uint16_t rgb888_to_rgb565_rounded(uint8_t r, uint8_t g, uint8_t b) {
  // Round-to-nearest so the dimmest non-zero level lands on the nearest
  // 5/6-bit step instead of always flooring to 0. Recovers some of the
  // resolution lost to PANEL_BIT_DEPTH=5.
  const uint16_t r5 = (r * 31u + 127u) / 255u;
  const uint16_t g6 = (g * 63u + 127u) / 255u;
  const uint16_t b5 = (b * 31u + 127u) / 255u;
  return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

inline void interp(const Stop& a, const Stop& b, int i,
                   uint8_t* r, uint8_t* g, uint8_t* bl) {
  const int span = b.pos - a.pos;
  const int t    = i      - a.pos;
  *r  = static_cast<uint8_t>(a.r + ((b.r - a.r) * t) / span);
  *g  = static_cast<uint8_t>(a.g + ((b.g - a.g) * t) / span);
  *bl = static_cast<uint8_t>(a.b + ((b.b - a.b) * t) / span);
}

// Render `region`'s stops into `out[0..len)`. If the region is empty (no
// stops), zero-fill — that's the "black on wrong API usage" guarantee.
//
// Stop values are treated as PERCEPTUAL (sRGB-ish), not linear-light:
// what the artist wrote is what we want the eye to see. Earlier versions
// applied a 2.2 gamma here, but that collapsed the dim half of every
// ramp to RGB565 black after quantization (see "FG ramps black on the
// left half" diagnostic, May 2026). Round-to-nearest packing in
// rgb888_to_rgb565_rounded already recovers the dim-end resolution we
// lost dropping bit depth from 6 to 5; per-renderer envelope math
// (e.g. starfield's shimmer * envelope) provides the brightness curve
// when one is needed.
void build_region(const Region& region, uint16_t* out, int len) {
  if (region.count == 0) {
    for (int i = 0; i < len; ++i) out[i] = 0x0000;
    return;
  }
  int seg = 0;
  for (int i = 0; i < len; ++i) {
    while (seg + 1 < region.count - 1 && i > region.stops[seg + 1].pos) {
      ++seg;
    }
    uint8_t r, g, b;
    interp(region.stops[seg], region.stops[seg + 1], i, &r, &g, &b);
    out[i] = rgb888_to_rgb565_rounded(r, g, b);
  }
}

}  // namespace

void init_all() {
  for (int p = 0; p < static_cast<int>(Id::COUNT); ++p) {
    build_region(kDefs[p].bg, &g_tables[p][0],       BG_LEN);
    build_region(kDefs[p].fg, &g_tables[p][FG_BASE], FG_LEN);
  }
}

uint16_t fg(Id p, uint8_t br6) {
  return g_tables[static_cast<int>(p)][FG_BASE + (br6 & (FG_LEN - 1))];
}

uint16_t bg(Id p, uint16_t idx, uint16_t shift) {
  // idx and shift can be arbitrary; the modulo confines us to the cyclic
  // BG range. Adding before mod keeps the operation cheap and obviously
  // correct for any non-negative inputs.
  const uint16_t i = static_cast<uint16_t>((idx + shift) % BG_LEN);
  return g_tables[static_cast<int>(p)][i];
}

}  // namespace palette

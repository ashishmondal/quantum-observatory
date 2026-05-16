// SECTION_NINE — see include/themes/section_nine_theme.h.

#include "themes/section_nine_theme.h"

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

// SECTION_NINE — GitS 2017 holographic-noir HUD (THEME.md §2.6).
// HSV-disciplined: hot pink fg lives at H≈325°/V≈95% (#F81094 →
// 0xF892); halos drop V to ~10–15% at the same hue so they read as
// magenta glow, not muddy black. Cyan secondary at H≈190° is the
// only non-pink hue and is reserved for STATUS_INFO + accent pops.
constexpr uint16_t kInks[static_cast<int>(Ink::COUNT)] = {
  /*CHROME         */ 0xFFFF,   // small clock chrome — white pop
  /*CHROME_HALO    */ 0x0000,
  /*HEADER         */ 0xF892,   // hot pink — theme identity
  /*HEADER_HALO    */ 0x2002,   // ~12% V deep magenta — glow shadow
  /*HEADER_GLOW    */ 0x2002,   // NEON_OUTLINE 2nd halo color
  /*HEADER_DIM     */ 0x4008,   // pulse-low for generic header (~30% V pink)
  /*BODY           */ 0xF892,
  /*BODY_HALO      */ 0x2002,
  /*BODY_GLOW      */ 0x2002,
  /*ACCENT         */ 0xFFFF,   // white pop
  /*ACCENT_MAGENTA */ 0xF81F,   // saturated magenta — second accent
  /*ALERT          */ 0xF800,   // bright red — priority callout
  /*GHOST          */ 0x0083,   // ~10% V deep teal — LCD ghost
  /*DIVIDER        */ 0x4008,   // dim pink — 1-px separators
  /*GIANT_DIGITS   */ 0xF892,
  /*STATUS_OK      */ 0x05DF,   // bright cyan — overhead / healthy
  /*STATUS_OK_DIM  */ 0x010A,   // dim cyan — pulse-low for OK header
  /*STATUS_WARN    */ 0xF892,   // hot pink — countdown / daylight wash
  /*STATUS_WARN_DIM*/ 0x4008,   // dim pink — pulse-low for WARN header
  /*STATUS_INFO    */ 0x05DF,   // cyan — info data
  /*STATUS_STALE   */ 0x4008,   // dim pink — no fresh data ("WAIT")
  /*STATUS_DIM     */ 0x2002,   // very dim — below horizon
  /*LABEL          */ 0x4008,   // ~30% V pink — dim label
  /*VALUE          */ 0xF892,   // hot pink — bright value
  /*SAFETY         */ 0x4000,   // deep red — night / thermal_safe
};

// Ladder order: MICRO, BODY, HEADER, CLOCK (FR-15.9). Reuses the
// Org_01 + Pixel Operator pair already bundled for BLADE_RUNNER /
// LCARS — zero new TTFs (THEME.md §5 three-font budget intact).
constexpr const GFXfont* kFonts[static_cast<int>(FontRole::COUNT)] = {
  /*MICRO */ &Tiny3x3a2pt7b,
  /*BODY  */ &Org_01,                  // 5×6 sans w/ true lowercase — HUD feel
  /*HEADER*/ &PixelOperator88pt7b,
  /*CLOCK */ &digital_7__mono_14pt7b,
};

// NEON_OUTLINE only. SCANLINES deliberately omitted — the bg motif
// already owns the horizontal-stripe identity, stacking the post-
// overlay scanlines on top would muddy the hologram band reading.
constexpr uint32_t kHintMask =
    1u << static_cast<uint32_t>(Hint::NEON_OUTLINE);

// Duotone bg ramp (THEME.md §6.1). BG_HIGHLIGHT pinned to the same
// hot-pink hue as kInks[HEADER]; BG_SHADOW is the deep-teal
// complementary; WHITE anchor leans rose so highlights don't drift
// to cyan-white under §6.9 blend.
constexpr BgRamp kBgRamp = {
  {0x000000, 0x002038, 0xFF1493, 0xFFE0F0},
};

// Hand-pixel 5×7 katakana glyphs that lead each climbing shaft. Six
// is plenty for visual variety at 12 shafts; the set is fixed so the
// motion stays deterministic. Each row's leftmost 5 bits encode the
// scanline; bit 4 is the leftmost pixel.
constexpr int kGlyphRows  = 7;
constexpr int kGlyphCols  = 5;
constexpr int kGlyphCount = 6;
constexpr uint8_t kGlyphs[kGlyphCount][kGlyphRows] = {
  // ロ (ro) — solid rectangle
  { 0b11111, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11111 },
  // ニ (ni) — two horizontal strokes
  { 0b00000, 0b11111, 0b00000, 0b00000, 0b00000, 0b11111, 0b00000 },
  // エ (e)  — I-beam
  { 0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111 },
  // ヨ (yo) — three right-facing horizontals on a stem
  { 0b11111, 0b00001, 0b11111, 0b00001, 0b00001, 0b11111, 0b00000 },
  // ク (ku) — diagonal sweep
  { 0b11110, 0b00001, 0b00010, 0b00100, 0b00100, 0b01000, 0b01000 },
  // シ (shi) — three drops on the left + rising sweep
  { 0b10000, 0b00000, 0b10000, 0b00000, 0b00001, 0b00010, 0b11100 },
};

}  // namespace

SectionNineTheme::SectionNineTheme()
    : Theme(Id::SECTION_NINE,
            "section_nine",
            "SECTION 9",
            kInks,
            kFonts,
            kHintMask,
            "|", "|",
            &kBgRamp) {}

uint32_t SectionNineTheme::next_rand() {
  m_rng = m_rng * 1664525u + 1013904223u;
  return m_rng;
}

void SectionNineTheme::respawn(int i, bool initial) {
  // x: glyph-left in [0, PANEL_WIDTH - kGlyphCols]. The 2-px-wide
  // body/tail trail beneath sits at (x+1, x+2) — still inside the
  // glyph's footprint, so the whole shaft fits without per-frame
  // edge clipping of the trail.
  const uint32_t r1 = next_rand();
  m_x[i] = static_cast<int8_t>((r1 >> 8) % (PANEL_WIDTH - kGlyphCols + 1));
  // speed: 1 or 2 px/frame, 2:1 in favour of the slower speed so the
  // field has a clear background layer + a few fast risers.
  m_speed[i] = ((next_rand() >> 16) % 3u) == 0u ? 2u : 1u;
  // color: 4:1 magenta:cyan — cyan accents are sparse enough to read
  // as data variation, dense enough to never miss for many seconds.
  m_color[i] = ((next_rand() >> 24) % 5u) == 0u ? 1u : 0u;
  // glyph: uniform pick over the small katakana table.
  m_glyph[i] = static_cast<uint8_t>((next_rand() >> 12) % kGlyphCount);
  // y: start below the bottom edge, walked back along the climb axis
  // by a stagger so all 12 shafts don't enter the field at once.
  // initial=true spreads across the full panel height; afterwards a
  // shorter delay keeps the column density steady.
  const uint32_t span  = initial ? PANEL_HEIGHT * 2u : PANEL_HEIGHT;
  const uint32_t delay = (next_rand() >> 8) % span;
  m_y[i] = static_cast<int8_t>(PANEL_HEIGHT - 1 + static_cast<int16_t>(delay));
}

void SectionNineTheme::init_clock_bg() {
  m_rng = 0xD3E72A91u;
  for (int i = 0; i < kShafts; ++i) respawn(i, /*initial=*/true);
  m_frame_count = 0;
}

// Climbing katakana glyphs. Each shaft is a single 5×7 hand-pixel
// katakana that cycles independently every ~8 frames (offset per-
// shaft so the field doesn't flicker in unison) — the Matrix-code
// feel of cycling data, without the underlying smear-trail that
// previously read as solid magenta bars and competed with the giant
// digits for attention.
//
// Tear: every ~120 frames (≈ 7.5 s) a real horizontal screen tear
// fires — a single scanline seam splits the panel and rows above /
// below shift in OPPOSITE directions for one frame, producing the
// CRT-style displacement of vertical strokes you see on a glitching
// signal (not a translation of the whole field). Seam y, shift
// magnitude, and sign all rotate deterministically per tear event.
void SectionNineTheme::render_clock_bg(Adafruit_Protomatter& matrix,
                                       uint32_t now_ms) {
  matrix.fillScreen(0x0000);
  (void)now_ms;
  ++m_frame_count;

  constexpr uint32_t kTearPeriod  = 120u;
  constexpr uint32_t kGlyphSwapMs = 8u;     // base glyph dwell, frames
  constexpr int16_t  kHeadH       = kGlyphRows;  // 7 — also total length

  // ── Per-row tear LUT ─────────────────────────────────────────
  // 0 on non-tear frames. On a tear frame: seam y splits the panel,
  // rows above shift +dx, rows below shift -dx. Two seams stack
  // every fourth tear event for the heavy-glitch flavour.
  int8_t row_dx[PANEL_HEIGHT] = {};
  if ((m_frame_count % kTearPeriod) == 0u) {
    const uint32_t evt = m_frame_count / kTearPeriod;
    // 6 deterministic seam y values spread across the panel.
    static constexpr int8_t kSeams[6] = { 6, 11, 15, 19, 23, 27 };
    const int8_t seam_a = kSeams[evt % 6u];
    // Shift magnitude alternates 2 / 3 / 2 / 3 ; sign alternates
    // every event so tears feel like noise, not drift.
    const int8_t mag    = (evt & 1u) ? 3 : 2;
    const int8_t sign   = ((evt >> 1) & 1u) ? -1 : 1;
    for (int16_t y = 0; y < PANEL_HEIGHT; ++y) {
      row_dx[y] = static_cast<int8_t>((y < seam_a ? +mag : -mag) * sign);
    }
    // Every fourth event stacks a second seam — the lower half flips
    // again, producing three bands instead of two.
    if ((evt % 4u) == 0u) {
      const int8_t seam_b = kSeams[(evt + 3u) % 6u];
      if (seam_b > seam_a) {
        for (int16_t y = seam_b; y < PANEL_HEIGHT; ++y) {
          row_dx[y] = static_cast<int8_t>(-row_dx[y]);
        }
      }
    }
  }

  const uint16_t m_bright  = ink(Ink::HEADER);        // hot pink
  const uint16_t c_bright  = ink(Ink::STATUS_OK);     // cyan

  for (int i = 0; i < kShafts; ++i) {
    // Advance head.
    m_y[i] = static_cast<int8_t>(m_y[i] - static_cast<int16_t>(m_speed[i]));
    // Respawn when the glyph has fully cleared the top edge.
    if (m_y[i] < -kHeadH) {
      respawn(i, /*initial=*/false);
      continue;
    }

    // Glyph swap — each shaft cycles to a new katakana every
    // kGlyphSwapMs frames, with a per-shaft phase offset so the
    // field doesn't flicker in lockstep. The chosen glyph is a
    // deterministic function of (frame_count, shaft index), so
    // there's no per-frame RNG churn.
    const uint32_t swap_tick =
        (m_frame_count + static_cast<uint32_t>(i) * 3u) / kGlyphSwapMs;
    const uint8_t glyph_idx = static_cast<uint8_t>(
        (m_glyph[i] + swap_tick) % kGlyphCount);

    const bool     is_cyan = m_color[i] != 0u;
    const uint16_t bright  = is_cyan ? c_bright : m_bright;

    const int16_t  glyph_x = m_x[i];
    const int16_t  head_y  = m_y[i];
    const uint8_t* g       = kGlyphs[glyph_idx];

    // Blit the 5×7 katakana head, applying per-row tear shift.
    for (int gy = 0; gy < kGlyphRows; ++gy) {
      const int16_t py = head_y + gy;
      if (py < 0 || py >= PANEL_HEIGHT) continue;
      const int16_t rdx = static_cast<int16_t>(row_dx[py]);
      const uint8_t row = g[gy];
      for (int gx = 0; gx < kGlyphCols; ++gx) {
        if ((row & (1u << (kGlyphCols - 1 - gx))) == 0u) continue;
        const int16_t px = glyph_x + gx + rdx;
        if (px >= 0 && px < PANEL_WIDTH) {
          matrix.drawPixel(px, py, bright);
        }
      }
    }
  }
}

// ── FR-10.7 signature melody ────────────────────────────────────
// "Making of a Cyborg" descent — four-note minor fall A4 → G4 → F4
// → E4. Last note held to leave the phrase unresolved on the
// dominant, mirroring Kawai's choral-loop pedal point. Total =
// 220+220+220+600 = 1260 ms (under the 1500 ms FR-10.7 cap).
namespace {
constexpr buzzer::Note kMelody[] = {
  { 440, 220 },   // A4
  { 392, 220 },   // G4
  { 349, 220 },   // F4
  { 330, 600 },   // E4 — held, unresolved
};
}  // namespace

Melody SectionNineTheme::melody() const {
  return { kMelody, sizeof(kMelody) / sizeof(kMelody[0]) };
}

SectionNineTheme g_section_nine_theme;

}  // namespace theme

// APOLLO_AMBER — see include/themes/apollo_amber_theme.h.

#include "themes/apollo_amber_theme.h"

#include <Adafruit_GFX.h>
#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>
#include <Fonts/Tiny3x3a2pt7b.h>

#include "buzzer.h"
#include "color_palette.h"
#include "config.h"
#include "fonts/digital_7__mono_14pt7b.h"
#include "fonts/press_start_2p_8pt7b.h"

namespace theme {

namespace {

// APOLLO_AMBER inks — values mirror the existing hardcoded literals
// so the T.3 scene refactor lands as a pure search-and-replace:
//   - chrome (gfx_text.h draw_clock_chrome defaults): white on black halo
//   - body (giant_clock_scene date strip):            deep amber 0xF940
//   - ghost (giant_clock_scene LCD ghost):            ~3% neutral grey
//   - divider (giant_clock_scene divider hline):      dim warm green 0x0300
//   - giant_digits (giant_clock_scene live HH:MM):    white 0xFFFF
//   - status_* / label / value / safety: extracted in T.3b from the
//     constexpr literals previously duplicated across iss_pass,
//     jupiter_visibility, moon_phase, constellation_now, night, and
//     thermal_safe. Apollo values match the originals byte-for-byte
//     so refactoring those scenes produces zero visual delta.
constexpr uint16_t kInks[static_cast<int>(Ink::COUNT)] = {
  /*CHROME         */ 0xFFFF,
  /*CHROME_HALO    */ 0x0000,
  /*HEADER         */ 0xFFFF,
  /*HEADER_HALO    */ 0x0000,
  /*HEADER_GLOW    */ 0x0000,   // Apollo doesn't NEON_OUTLINE; unused
  /*HEADER_DIM     */ 0x0000,
  /*BODY           */ 0xF940,   // deep amber, matches giant_clock date
  /*BODY_HALO      */ 0x0000,
  /*BODY_GLOW      */ 0x0000,
  /*ACCENT         */ 0xFFFF,   // white pop (THEME.md §2.1)
  /*ACCENT_MAGENTA */ 0xF81F,
  /*ALERT          */ 0xF800,
  /*GHOST          */ 0x0821,   // ~3% neutral grey LCD ghost
  /*DIVIDER        */ 0x0300,   // dim warm green
  /*GIANT_DIGITS   */ 0xFFFF,
  /*STATUS_OK      */ 0x07E0,
  /*STATUS_OK_DIM  */ 0x0140,
  /*STATUS_WARN    */ 0xFD20,
  /*STATUS_WARN_DIM*/ 0x6A00,
  /*STATUS_INFO    */ 0x07FF,
  /*STATUS_STALE   */ 0xC100,
  /*STATUS_DIM     */ 0x630C,
  /*LABEL          */ 0x31A6,
  /*VALUE          */ 0xCE79,
  /*SAFETY         */ 0x4000,
};

// Ladder order: MICRO, BODY, HEADER, CLOCK (FR-15.9).
constexpr const GFXfont* kFonts[static_cast<int>(FontRole::COUNT)] = {
  /*MICRO */ &Tiny3x3a2pt7b,
  /*BODY  */ &Picopixel,
  /*HEADER*/ &PressStart2P8pt7b,         // APOLLO MOCR chunky 8x8 (FR-4.1)
  /*CLOCK */ &digital_7__mono_14pt7b,
};

constexpr uint32_t kHintMask =
    1u << static_cast<uint32_t>(Hint::GIANT_DIGIT_GHOST);

}  // namespace

ApolloAmberTheme::ApolloAmberTheme()
    : Theme(Id::APOLLO_AMBER,
            "apollo_amber",
            "APOLLO AMBER",
            kInks,
            kFonts,
            kHintMask,
            "[", "]",
            /*bg_ramp=*/nullptr) {}

// ── Animated clock background ────────────────────────────────────
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
// 63 - (offset * 63) / 192.
void ApolloAmberTheme::render_clock_bg(Adafruit_Protomatter& matrix,
                                       uint32_t now_ms) {
  matrix.fillScreen(0x0000);

  constexpr uint32_t kRowMs    = 125;                   // 64 px in 125 ms
  constexpr int      kPathLen  = PANEL_WIDTH * PANEL_HEIGHT;
  constexpr uint32_t kPeriodMs = kRowMs * PANEL_HEIGHT; // full screen
  constexpr int      kTrail    = PANEL_WIDTH * 3;       // 3 scan lines

  const uint32_t t       = now_ms % kPeriodMs;
  const int      head_lp = static_cast<int>(
      (static_cast<uint64_t>(t) * kPathLen) / kPeriodMs);

  for (int off = kTrail; off >= 0; --off) {
    int lp = head_lp - off;
    if (lp < 0) lp += kPathLen;
    const int x = lp % PANEL_WIDTH;
    const int y = lp / PANEL_WIDTH;
    const uint8_t br = static_cast<uint8_t>(
        63 - (static_cast<int>(off) * 63) / kTrail);
    if (br == 0) continue;
    matrix.drawPixel(x, y, palette::fg(palette::Id::STAR_AMBER, br));
  }
}

// ── FR-10.7 signature melody ────────────────────────────────────
// "James Horner Signal" — lonely rising hero interval (Apollo 13).
// Total: 150 + 50 + 150 + 300 = 650 ms wall-clock (well under the
// 1500 ms cap). Grace note (11087 Hz / 50 ms) sits between the two
// 8372 Hz tonic statements before the climb to 12543 Hz.
namespace {
constexpr buzzer::Note kMelody[] = {
  { 8372, 150 },
  {11087,  50 },   // grace
  { 8372, 150 },
  {12543, 300 },
};
}  // namespace

Melody ApolloAmberTheme::melody() const {
  return { kMelody, sizeof(kMelody) / sizeof(kMelody[0]) };
}

ApolloAmberTheme g_apollo_amber_theme;

}  // namespace theme

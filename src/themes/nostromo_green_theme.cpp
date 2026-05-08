// NOSTROMO_GREEN — see include/themes/nostromo_green_theme.h.

#include "themes/nostromo_green_theme.h"

#include <Adafruit_GFX.h>
#include <Adafruit_Protomatter.h>
#include <Fonts/TomThumb.h>
#include <Fonts/Tiny3x3a2pt7b.h>

#include "buzzer.h"
#include "config.h"
#include "fonts/digital_7__mono_14pt7b.h"
#include "fonts/vt323_8pt7b.h"

namespace theme {

namespace {

// NOSTROMO_GREEN — 80s Alien CRT (THEME.md §2.2). Phosphor green
// dominates (chrome / header / body / status_ok); yellow carries
// warn + alert (matches the MU/TH/UR caution-stripe palette); pale
// green-white pops as ACCENT. SAFETY stays a deep red even under
// green CRT — it's a hardware-safety signal.
constexpr uint16_t kInks[static_cast<int>(Ink::COUNT)] = {
  /*CHROME         */ 0x07E0,
  /*CHROME_HALO    */ 0x0000,
  /*HEADER         */ 0x07E0,
  /*HEADER_HALO    */ 0x0000,
  /*HEADER_GLOW    */ 0x0000,
  /*HEADER_DIM     */ 0x0140,
  /*BODY           */ 0x07E0,
  /*BODY_HALO      */ 0x0000,
  /*BODY_GLOW      */ 0x0000,
  /*ACCENT         */ 0xDFFB,
  /*ACCENT_MAGENTA */ 0xFFE0,
  /*ALERT          */ 0xFFE0,
  /*GHOST          */ 0x0040,
  /*DIVIDER        */ 0x0140,
  /*GIANT_DIGITS   */ 0x07E0,
  /*STATUS_OK      */ 0x07E0,
  /*STATUS_OK_DIM  */ 0x0140,
  /*STATUS_WARN    */ 0xFFE0,
  /*STATUS_WARN_DIM*/ 0x4200,
  /*STATUS_INFO    */ 0x07E0,
  /*STATUS_STALE   */ 0x2100,
  /*STATUS_DIM     */ 0x0140,
  /*LABEL          */ 0x0560,
  /*VALUE          */ 0x07E0,
  /*SAFETY         */ 0x4000,
};

// VT323 binds to HEADER per THEME.md §2.2. BODY is TomThumb — 3×5,
// no descenders, thinner grid than Picopixel — to read as a CRT
// terminal under green phosphor (THEME.md §5).
constexpr const GFXfont* kFonts[static_cast<int>(FontRole::COUNT)] = {
  /*MICRO */ &Tiny3x3a2pt7b,
  /*BODY  */ &TomThumb,
  /*HEADER*/ &VT323_Regular12pt7b,
  /*CLOCK */ &digital_7__mono_14pt7b,
};

constexpr uint32_t kHintMask =
    1u << static_cast<uint32_t>(Hint::CURSOR_BLOCK);

constexpr BgRamp kBgRamp = {
  {0x000000, 0x003000, 0x40FF40, 0xD8FFD8},
};

}  // namespace

NostromoGreenTheme::NostromoGreenTheme()
    : Theme(Id::NOSTROMO_GREEN,
            "nostromo_green",
            "NOSTROMO GREEN",
            kInks,
            kFonts,
            kHintMask,
            ">", "_",
            &kBgRamp) {}

uint32_t NostromoGreenTheme::next_rand() {
  m_rng = m_rng * 1664525u + 1013904223u;
  return m_rng;
}

void NostromoGreenTheme::init_clock_bg() {
  m_rng = 0xA5F03C2Du;
  // Step periods picked from a small set so visually-distinct fast/slow
  // drops coexist; phase staggers initial positions so the first frame
  // doesn't show a synchronized line.
  static constexpr uint16_t kStepChoices[4] = { 25, 40, 65, 100 };
  for (int i = 0; i < PANEL_WIDTH; ++i) {
    m_col_step_ms[i] = kStepChoices[next_rand() & 0x3];
    m_col_phase[i]   = static_cast<uint8_t>((next_rand() >> 8) & 0x3F);
  }
}

// Every-other column hosts an independent drop. Head ink is the
// bright STATUS_OK phosphor; below it a 4-row gradient (DIVIDER →
// sparse DIVIDER) trails behind. The CYCLE adds a few rows of
// off-panel dead time per column so the field doesn't read as a
// solid wall of green.
void NostromoGreenTheme::render_clock_bg(Adafruit_Protomatter& matrix,
                                         uint32_t now_ms) {
  matrix.fillScreen(0x0000);

  const uint16_t head = ink(Ink::STATUS_OK);
  const uint16_t tail = ink(Ink::DIVIDER);
  constexpr int CYCLE = PANEL_HEIGHT + 10;

  for (int x = 1; x < PANEL_WIDTH; x += 2) {
    const uint16_t step  = m_col_step_ms[x];
    const uint8_t  phase = m_col_phase[x];
    const int      pos   = static_cast<int>(((now_ms / step) + phase) % CYCLE);

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

// ── FR-10.7 signature melody ────────────────────────────────────
// "Jerry Goldsmith Echo" — dissonant two-note call (Alien). High
// → low, total 1000 ms.
namespace {
constexpr buzzer::Note kMelody[] = {
  { 9397, 400 },
  { 8372, 600 },
};
}  // namespace

Melody NostromoGreenTheme::melody() const {
  return { kMelody, sizeof(kMelody) / sizeof(kMelody[0]) };
}

NostromoGreenTheme g_nostromo_green_theme;

}  // namespace theme

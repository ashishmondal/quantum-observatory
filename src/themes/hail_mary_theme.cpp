// HAIL_MARY — see include/themes/hail_mary_theme.h.

#include "themes/hail_mary_theme.h"

#include <Adafruit_GFX.h>
#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>
#include <Fonts/Tiny3x3a2pt7b.h>

#include "buzzer.h"
#include "config.h"
#include "fonts/digital_7__mono_14pt7b.h"
#include "fonts/pixel_operator_8pt7b.h"
#include "scenes/hail_mary_scene.h"

namespace theme {

namespace {

// HSV plan (user-memory color-perception notes): identity hue is
// Petrova crimson H≈345°, V≈100 % (#FF2050 → 0xF90A); halos/dims keep
// the hue at V 10–30 %. Body is xenonite silver-blue H≈215°, S≈35 %
// (#A0C8FF → 0xA65F) so data never competes with the crimson header.
// Halos sit ~130° away in hue (deep xenonite blue, V≈19 %) — a same-hue
// crimson halo smeared into the glyphs and killed legibility.
constexpr uint16_t kInks[static_cast<int>(Ink::COUNT)] = {
  /*CHROME         */ 0xFFFF,
  /*CHROME_HALO    */ 0x0000,
  /*HEADER         */ 0xF90A,   // Petrova crimson
  /*HEADER_HALO    */ 0x00A6,   // ~19 % V xenonite blue, H≈215°
  /*HEADER_GLOW    */ 0x00A6,
  /*HEADER_DIM     */ 0x4803,   // ~30 % V crimson pulse-low
  /*BODY           */ 0xA65F,   // xenonite silver-blue
  /*BODY_HALO      */ 0x0000,
  /*BODY_GLOW      */ 0x0000,
  /*ACCENT         */ 0xFFFF,
  /*ACCENT_MAGENTA */ 0xF90A,
  /*ALERT          */ 0xF800,
  /*GHOST          */ 0x0862,   // ~5 % V xenonite — same hue as the live digits
  /*DIVIDER        */ 0x3002,
  /*GIANT_DIGITS   */ 0xC6FF,   // pale xenonite
  /*STATUS_OK      */ 0x07E8,
  /*STATUS_OK_DIM  */ 0x0144,
  /*STATUS_WARN    */ 0xFD20,   // Hail Mary hull-light amber
  /*STATUS_WARN_DIM*/ 0x6A00,
  /*STATUS_INFO    */ 0x5D1F,
  /*STATUS_STALE   */ 0x6004,
  /*STATUS_DIM     */ 0x4208,
  /*LABEL          */ 0x31A8,
  /*VALUE          */ 0xC6FF,
  /*SAFETY         */ 0x4000,
};

constexpr const GFXfont* kFonts[static_cast<int>(FontRole::COUNT)] = {
  /*MICRO */ &Tiny3x3a2pt7b,
  /*BODY  */ &Picopixel,
  /*HEADER*/ &PixelOperator88pt7b,
  /*CLOCK */ &digital_7__mono_14pt7b,
};

constexpr uint32_t kHintMask =
    1u << static_cast<uint32_t>(Hint::GIANT_DIGIT_GHOST);

constexpr BgRamp kBgRamp = {
  {0x000000, 0x200818, 0xFF2050, 0xFFE8F0},
};

// "Amaze, amaze, amaze" — Rocky's chord-speech as a rising/falling
// A-major arpeggio. 90×3 + 60 + 90×2 + 200 = 710 ms.
constexpr buzzer::Note kMelody[] = {
  {  880, 90 }, { 1109, 90 }, { 1319, 90 }, { 0, 60 },
  { 1319, 90 }, { 1109, 90 }, {  880, 200 },
};

}  // namespace

HailMaryTheme::HailMaryTheme()
    : Theme(Id::HAIL_MARY,
            "hail_mary",
            "HAIL MARY",
            kInks,
            kFonts,
            kHintMask,
            "<", ">",
            &kBgRamp) {}

void HailMaryTheme::render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms) {
  matrix.fillScreen(0x0000);
  HailMaryScene::draw_petrova(matrix, now_ms);
}

Melody HailMaryTheme::melody() const {
  return { kMelody, sizeof(kMelody) / sizeof(kMelody[0]) };
}

HailMaryTheme g_hail_mary_theme;

}  // namespace theme

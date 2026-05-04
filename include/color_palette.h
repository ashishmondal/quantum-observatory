// Smooth-color palette system with split layout for palette cycling.
//
// Every palette is a 256-entry RGB565 table with a fixed split layout:
//
//     [  0 .. 191 ]   "background" region — cyclic. Designed so that
//                     entry 191 visually flows back into entry 0; renderers
//                     animate by walking pixel index + a global shift,
//                     wrapping mod 192.  See palette::bg().
//
//     [192 .. 255 ]   "foreground" region — linear brightness ramp from
//                     black at 192 to peak color at 255. Not rotated.
//                     Used by stars, text, sprites — anything that needs
//                     a stable per-pixel "how bright" lookup.  See palette::fg().
//
// A palette declares which regions it actually uses (FG-only for stars,
// BG-only for nebula clouds, BOTH for an integrated scene). Unused regions
// are filled with black so that accidentally calling the wrong API just
// shows blank pixels instead of garbage colors.
//
// Tables are built once at boot (palette::init_all from setup()), then
// read-only on the render path. ~512 B SRAM per palette.

#pragma once

#include <stdint.h>

namespace palette {

enum class Id : uint8_t {
  STAR_WHITE    = 0,  // FG only — black -> warm white
  STAR_BLUE     = 1,  // FG only — black -> blue -> cyan -> blue-white
  STAR_AMBER    = 2,  // FG only — black -> red -> orange -> pale yellow
  NEBULA_CLOUDS = 3,  // BG only — cyclic deep-violet/magenta/teal cloud ramp
  NIGHT_SKY     = 4,  // FG only — uniform deep navy (entry 0..63 all same)
  COUNT
};

// Indices < FG_BASE belong to the cyclic BG region; >= FG_BASE belong to
// the linear FG ramp. Exposed so renderers don't hard-code the magic split.
static constexpr uint16_t BG_LEN  = 192;
static constexpr uint16_t FG_BASE = 192;
static constexpr uint16_t FG_LEN  = 64;

void init_all();

// Foreground lookup: brightness 0..63 → entries 192..255.
// Index 0 always maps to black (entry 192). Inline on the hot path.
uint16_t fg(Id p, uint8_t br6);

// Background lookup with cyclic shift. `idx` is the pixel's logical
// position in the 192-entry ramp (0..191); `shift` rotates the whole
// palette by that many steps. Renderers animate by ticking `shift` over
// time. Modulo is unavoidable (192 isn't a power of two) but it's a
// single 32-bit DIV per pixel, well within budget on RP2040.
uint16_t bg(Id p, uint16_t idx, uint16_t shift);

}  // namespace palette

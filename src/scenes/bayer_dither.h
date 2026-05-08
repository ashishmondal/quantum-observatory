// Bayer 8x8 ordered-dither helpers shared by the compositor's
// black-fade overlays (D.2 fade_black_layer, D.3 safety_overlay_layer).
//
// Why a destructive overlay rather than a true alpha blend:
//   Adafruit_Protomatter exposes no framebuffer readback. A real alpha
//   fade would need either a 4 KB shadow GFXcanvas16 the scenes write
//   through (and a Scene::render signature change to accept GFX&), or a
//   getPixel() patch into the library. The Bayer dither stamps 0x0000
//   over a deterministic subset of pixels each frame — no readback, no
//   off-screen buffer, retro pixel-art aesthetic.
//
// The 8x8 matrix values are scaled to 0..252 so the comparison
// `kBayer8[y%8][x%8] < alpha` is uniform across alpha=0..255.
//
// (added in phase D.2 as a private helper inside fade_black_layer; lifted
// to a shared header in phase D.3 when safety_overlay_layer reused the
// same primitive.)

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "config.h"

namespace bayer {

// Stamp 0x0000 over the live framebuffer wherever the 8x8 Bayer
// threshold is below `alpha` (0..255). alpha=0 is a no-op (returns
// immediately). alpha=255 blacks out every pixel.
//
// MUST be called only from Core 1 (it writes the live framebuffer).
// Per-pixel walk on a 64x32 panel is 2048 iterations — measured under
// 0.5 ms in steady state (the only branch is integer compare + maybe
// a drawPixel call); fits comfortably inside the 42 ms frame interval.
inline void apply_black_overlay(Adafruit_Protomatter& matrix, uint16_t alpha) {
  if (alpha == 0u) return;

  static constexpr uint8_t kBayer8[8][8] = {
    {  0, 128,  32, 160,   8, 136,  40, 168 },
    { 192,  64, 224,  96, 200,  72, 232, 104 },
    {  48, 176,  16, 144,  56, 184,  24, 152 },
    { 240, 112, 208,  80, 248, 120, 216,  88 },
    {  12, 140,  44, 172,   4, 132,  36, 164 },
    { 204,  76, 236, 108, 196,  68, 228, 100 },
    {  60, 188,  28, 156,  52, 180,  20, 148 },
    { 252, 124, 220,  92, 244, 116, 212,  84 },
  };

  for (int y = 0; y < PANEL_HEIGHT; ++y) {
    const uint8_t* row = kBayer8[y & 0x07];
    for (int x = 0; x < PANEL_WIDTH; ++x) {
      if (row[x & 0x07] < alpha) {
        matrix.drawPixel(x, y, 0x0000);
      }
    }
  }
}

}  // namespace bayer

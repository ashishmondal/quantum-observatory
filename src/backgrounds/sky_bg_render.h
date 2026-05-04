// Free-function sky renderer — shared between SkyBg (which feeds it
// the live RTC epoch) and SkyTimelapseScene (which feeds it a fake
// epoch). Same math, same colors. Pulling this out keeps the
// timelapse from having to mock out tod::now().
//
// (added alongside SkyBg in phase 6.5+ polish)

#pragma once

#include <math.h>
#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "sun_position.h"

namespace sky_bg_render {

namespace detail {

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

inline uint16_t lerp565(uint16_t a, uint16_t b, int i, int steps) {
  if (steps <= 0) return b;
  if (i <= 0) return a;
  if (i >= steps) return b;
  const int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  const int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  const int rr = ar + (br - ar) * i / steps;
  const int rg = ag + (bg - ag) * i / steps;
  const int rb = ab + (bb - ab) * i / steps;
  return static_cast<uint16_t>((rr << 11) | (rg << 5) | rb);
}

inline void draw_sun(Adafruit_Protomatter& matrix, int cx, int cy,
                     float altitude_deg) {
  // Four-tier disc, radius 4 (~9 px wide). Each tier carries a
  // top/bot color pair so we can paint a vertical gradient: cool/pale
  // on top, warm/red on the bottom. This mimics the way the rising
  // or setting sun looks through the atmosphere.
  uint16_t core_top, core_bot, mid_top, mid_bot, halo_top, halo_bot,
           wash_top, wash_bot;
  if (altitude_deg > 15.0f) {
    // High noon: nearly uniform pale white-yellow.
    core_top = rgb565(255, 250, 230); core_bot = rgb565(255, 245, 210);
    mid_top  = rgb565(255, 230, 170); mid_bot  = rgb565(255, 215, 140);
    halo_top = rgb565(220, 180, 110); halo_bot = rgb565(210, 160,  80);
    wash_top = rgb565(150, 110,  60); wash_bot = rgb565(140, 100,  50);
  } else if (altitude_deg > 5.0f) {
    // Mid sky: gentle vertical gradient.
    core_top = rgb565(255, 245, 210); core_bot = rgb565(255, 220, 160);
    mid_top  = rgb565(255, 215, 150); mid_bot  = rgb565(255, 170,  90);
    halo_top = rgb565(210, 160,  90); halo_bot = rgb565(200, 110,  50);
    wash_top = rgb565(140,  95,  50); wash_bot = rgb565(130,  70,  30);
  } else {
    // Sunrise / sunset: pronounced vertical gradient — pale yellow on
    // top, deep orange-red on the bottom.
    core_top = rgb565(255, 240, 190); core_bot = rgb565(255, 150,  60);
    mid_top  = rgb565(255, 200, 130); mid_bot  = rgb565(230,  90,  30);
    halo_top = rgb565(220, 140,  80); halo_bot = rgb565(170,  60,  20);
    wash_top = rgb565(140,  90,  50); wash_bot = rgb565(100,  35,  10);
  }

  constexpr int kR = 4;
  for (int dy = -kR; dy <= kR; ++dy) {
    for (int dx = -kR; dx <= kR; ++dx) {
      const int d2 = dx * dx + dy * dy;
      uint16_t top, bot;
      if (d2 <= 2)        { top = core_top; bot = core_bot; }
      else if (d2 <= 6)   { top = mid_top;  bot = mid_bot;  }
      else if (d2 <= 12)  { top = halo_top; bot = halo_bot; }
      else if (d2 <= 16)  { top = wash_top; bot = wash_bot; }
      else continue;
      const uint16_t c = lerp565(top, bot, dy + kR, 2 * kR);
      const int x = cx + dx, y = cy + dy;
      if (x >= 0 && x < PANEL_WIDTH && y >= 0 && y < PANEL_HEIGHT) {
        matrix.drawPixel(x, y, c);
      }
    }
  }
}

}  // namespace detail

// Draw the full sky (gradient + sun) for the given UTC epoch and
// observer location. Caller does not need to call matrix.show().
inline void draw(Adafruit_Protomatter& matrix, int32_t utc_epoch,
                 float latitude_deg, float longitude_deg) {
  using detail::rgb565;
  const sun::Position p =
      sun::compute(utc_epoch, latitude_deg, longitude_deg);

  // Gradient bands are intentionally dim — the panel runs hours per
  // day on the daytime band, so we trade vibrant blue for ~half the
  // sustained current draw. The sun (drawn on top) stays bright so it
  // still pops against the muted sky.
  uint16_t top, bot;
  if (p.altitude_deg > 10.0f) {
    // Day: deep navy → dusty mid-blue (was vivid sky-blue).
    top = rgb565(  2,  10,  30);
    bot = rgb565( 40,  70, 110);
  } else if (p.altitude_deg > 0.0f) {
    // Golden hour: muted indigo → warm amber.
    top = rgb565( 12,  25,  55);
    bot = rgb565(120,  70,  25);
  } else if (p.altitude_deg > -6.0f) {
    // Civil twilight: dim violet → dim red-orange.
    top = rgb565(  6,   6,  35);
    bot = rgb565( 90,  30,  15);
  } else if (p.altitude_deg > -12.0f) {
    // Nautical twilight.
    top = rgb565(  4,   4,  25);
    bot = rgb565( 25,  15,  50);
  } else {
    // Astronomical / deep night.
    top = rgb565(  2,   2,  10);
    bot = rgb565(  8,   8,  28);
  }

  for (int y = 0; y < PANEL_HEIGHT; ++y) {
    const uint16_t c = detail::lerp565(top, bot, y, PANEL_HEIGHT - 1);
    matrix.drawFastHLine(0, y, PANEL_WIDTH, c);
  }

  // Project the sun onto the panel facing south. We map azimuth
  // LINEARLY to x: az 60° → x=0, az 180° (south) → x=31.5,
  // az 300° → x=63. Linear (rather than sin-based) keeps the sun
  // moving at a steady horizontal pace; combined with the altitude
  // arc on y this produces a proper semi-elliptical sun path instead
  // of "dwelling at the edges, then dashing across the top".
  // sx is clamped so the radius-4 disc never gets cropped at the
  // panel edges at sunrise/sunset.
  if (p.altitude_deg > -3.0f &&
      p.azimuth_deg >= 45.0f && p.azimuth_deg <= 315.0f) {
    constexpr int kSunR = 4;
    const float xf = (p.azimuth_deg - 60.0f) * (63.0f / 240.0f);
    const float alt_clamped =
        p.altitude_deg < 0.0f ? 0.0f : p.altitude_deg;
    const float yf = 22.0f - alt_clamped * (22.0f / 90.0f);
    int sx = static_cast<int>(xf + 0.5f);
    int sy = static_cast<int>(yf < 0.0f ? 0.0f : yf + 0.5f);
    if (sx < kSunR) sx = kSunR;
    if (sx > PANEL_WIDTH - 1 - kSunR) sx = PANEL_WIDTH - 1 - kSunR;
    if (sy < kSunR) sy = kSunR;
    detail::draw_sun(matrix, sx, sy, p.altitude_deg);
  }
}

}  // namespace sky_bg_render

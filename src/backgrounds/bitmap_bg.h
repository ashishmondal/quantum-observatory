// Bitmap-backed background with palette cycling.
//
// A "bitmap" here is a PANEL_WIDTH x PANEL_HEIGHT array of palette
// indices (one byte per pixel). The byte's value selects which palette
// entry to draw:
//
//   index <  192   -> BG region (cyclic). Animated by adding a per-frame
//                     shift before lookup. Multiple disjoint sub-ranges
//                     of the BG region can each have their own shift /
//                     speed (Region table) so different parts of the
//                     image can flow at different rates and directions.
//
//   index >= 192   -> FG region. Static; drawn as palette::fg(p, idx-192).
//                     Use for sprite-like static highlights (stars, text
//                     pixels, anything that shouldn't move).
//
// Two ways to populate the bitmap:
//   init_generated(fn, ...)  - call fn(x, y) for every cell at init.
//                              No per-frame cost; bitmap lives in the
//                              instance's owned 2 KB buffer afterwards.
//   init_from(buf, ...)      - copy from an externally-stored buffer
//                              (e.g. a constexpr table in flash).
//
// Render cost: O(W*H) per frame (~2048 lookups). Each pixel does a
// single byte read, at most MAX_REGIONS (currently 4) integer compares,
// one mod, and one drawPixel. Comfortably below frame budget at 24 FPS.

#pragma once

#include <stdint.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "color_palette.h"
#include "config.h"

class BitmapBg {
public:
  // One cyclic sub-range. `start` is the first BG palette index
  // (0..191) covered by this region; `length` is the number of
  // consecutive indices that cycle together; `speed` is signed steps
  // per second (+ scrolls forward, - scrolls backward, 0 = static).
  // Regions MUST NOT overlap and the union MUST stay within 0..191.
  struct Region {
    uint8_t start;
    uint8_t length;
    int16_t speed;
  };

  using PixelFn = uint8_t (*)(int x, int y);

  // Generate the bitmap in place from a per-pixel function. Region
  // table is copied into the instance (no aliasing the caller's array).
  void init_generated(PixelFn fn,
                      palette::Id pal,
                      const Region* regions, uint8_t region_count) {
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        m_pixels[y * W + x] = fn(x, y);
      }
    }
    set_meta(pal, regions, region_count);
  }

  // Copy a 2 KB pixel buffer in. Useful for flash-resident tables.
  void init_from(const uint8_t* pixels,
                 palette::Id pal,
                 const Region* regions, uint8_t region_count) {
    memcpy(m_pixels, pixels, sizeof(m_pixels));
    set_meta(pal, regions, region_count);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    // Precompute each region's shift once per frame (cheap; max 4
    // regions). Doing it per pixel would burn ~8 K mods per frame for
    // no gain.
    uint8_t shifts[MAX_REGIONS] = {0};
    for (uint8_t r = 0; r < m_region_count; ++r) {
      const Region& reg = m_regions[r];
      if (reg.length == 0) continue;
      // (now_ms * speed) / 1000 reduced mod length, sign-safe.
      int32_t s = (static_cast<int32_t>(now_ms) * reg.speed) / 1000;
      int32_t m = s % reg.length;
      if (m < 0) m += reg.length;
      shifts[r] = static_cast<uint8_t>(m);
    }

    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const uint8_t b = m_pixels[y * W + x];
        if (b >= palette::FG_BASE) {
          // Static FG pixel: brightness in low 6 bits.
          matrix.drawPixel(x, y,
              palette::fg(m_pal, static_cast<uint8_t>(b - palette::FG_BASE)));
          continue;
        }

        // Find the region this BG index belongs to. With <=4 regions,
        // a linear scan beats any indexed-lookup table in cache cost.
        uint8_t cycled = b;
        for (uint8_t r = 0; r < m_region_count; ++r) {
          const Region& reg = m_regions[r];
          if (b >= reg.start && b < reg.start + reg.length) {
            const uint8_t local = static_cast<uint8_t>(b - reg.start);
            const uint8_t rotated =
                static_cast<uint8_t>((local + shifts[r]) % reg.length);
            cycled = static_cast<uint8_t>(reg.start + rotated);
            break;
          }
        }
        // palette::bg(p, idx, 0) is just a table lookup; we already
        // applied the shift above so pass 0 here.
        matrix.drawPixel(x, y, palette::bg(m_pal, cycled, 0));
      }
    }
  }

private:
  static constexpr int W = PANEL_WIDTH;
  static constexpr int H = PANEL_HEIGHT;
  static constexpr int MAX_REGIONS = 4;

  void set_meta(palette::Id pal,
                const Region* regions, uint8_t region_count) {
    m_pal = pal;
    m_region_count = region_count > MAX_REGIONS ? MAX_REGIONS : region_count;
    for (uint8_t i = 0; i < m_region_count; ++i) m_regions[i] = regions[i];
  }

  uint8_t      m_pixels[W * H]{};
  palette::Id  m_pal           = palette::Id::NEBULA_CLOUDS;
  Region       m_regions[MAX_REGIONS]{};
  uint8_t      m_region_count  = 0;
};

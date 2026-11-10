// deep_sky scene (FR-21.8) — procedural pixel-art nebulae & galaxies.
//
// Offline. Each object is painted once (on switch) into a 64×24
// RGB565 buffer with integer-only value noise + radial falloffs, then
// blitted every frame with a few twinkling stars. 6 s showing the
// picture with its name, then 9 s with the picture dimmed behind
// three typed caption lines, then the next object.

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "scene.h"
#include "theme.h"

namespace deep_sky_detail {

struct Obj { const char* name; const char* dist; const char* fact; };

inline constexpr Obj kObjs[] = {
  { "ORION NEBULA", "1,344 LY AWAY",  "STAR NURSERY" },
  { "ANDROMEDA",    "2.5M LY AWAY",   "~1 TRILLION STARS" },
  { "PILLARS",      "~6,500 LY AWAY", "4 LY TALL GAS" },
  { "RING NEBULA",  "~2,600 LY AWAY", "DEAD STAR SHELL" },
  { "CRAB NEBULA",  "~6,500 LY AWAY", "SUPERNOVA 1054" },
  { "PLEIADES",     "444 LY AWAY",    "WINTER EVENINGS" },
};
inline constexpr uint8_t kCount = sizeof(kObjs) / sizeof(kObjs[0]);

constexpr int16_t kTop = 8;
constexpr int16_t kH   = PANEL_HEIGHT - kTop;  // 24 rows
constexpr uint32_t kShowMs = 6000;
constexpr uint32_t kTextMs = 9000;

inline uint8_t hash8(int32_t x, int32_t y, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<uint8_t>((h ^ (h >> 16)) & 0xFFu);
}

// Bilinear value noise on a 4-px lattice, two octaves → 0..255.
inline uint8_t noise(int16_t x, int16_t y, uint32_t seed) {
  auto oct = [&](int16_t xx, int16_t yy, int16_t cell, uint32_t s) -> int32_t {
    const int32_t gx = xx / cell, gy = yy / cell;
    const int32_t fx = ((xx % cell) * 256) / cell, fy = ((yy % cell) * 256) / cell;
    const int32_t a = hash8(gx, gy, s), b = hash8(gx + 1, gy, s);
    const int32_t c = hash8(gx, gy + 1, s), d = hash8(gx + 1, gy + 1, s);
    const int32_t top = a + (((b - a) * fx) >> 8);
    const int32_t bot = c + (((d - c) * fx) >> 8);
    return top + (((bot - top) * fy) >> 8);
  };
  return static_cast<uint8_t>((oct(x, y, 8, seed) * 2 + oct(x, y, 3, seed + 7)) / 3);
}

inline uint16_t pack(int32_t r, int32_t g, int32_t b) {
  auto c = [](int32_t v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
  return static_cast<uint16_t>(((c(r) & 0xF8) << 8) | ((c(g) & 0xFC) << 3) | (c(b) >> 3));
}

// 0..256 falloff for squared distance d2 against squared radius r2.
inline int32_t falloff(int32_t d2, int32_t r2) {
  return d2 >= r2 ? 0 : 256 - (d2 * 256) / r2;
}

}  // namespace deep_sky_detail

class DeepSkyScene : public Scene {
 public:
  const char* name() const override { return "deep_sky"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_idx      = 0;
    m_start_ms = 0;
    m_painted  = 0xFF;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    using namespace deep_sky_detail;
    if (m_start_ms == 0) m_start_ms = now_ms;
    uint32_t e = now_ms - m_start_ms;
    if (e >= kShowMs + kTextMs) {
      m_idx = static_cast<uint8_t>((m_idx + 1) % kCount);
      m_start_ms = now_ms;
      e = 0;
    }
    if (m_painted != m_idx) paint(m_idx);

    matrix.fillScreen(0x0000);
    const bool text_phase = e >= kShowMs;
    blit(matrix, text_phase ? 80 : 256, now_ms);
    cards::header(matrix, "DSO", now_ms);

    const Obj& o = kObjs[m_idx];
    if (!text_phase) {
      matrix.setFont(theme::font(theme::FontRole::BODY));
      matrix.setTextSize(1);
      const int16_t x = gfx::centered_x(matrix, o.name);
      gfx::draw_text_halo(matrix, x, PANEL_HEIGHT - 2, o.name,
                          theme::ink(theme::Ink::VALUE), 0x0000);
    } else {
      cards::Card c;
      cards::set(c, 0, theme::ink(theme::Ink::VALUE), "%s", o.name);
      cards::set(c, 1, theme::ink(theme::Ink::STATUS_INFO), "%s", o.dist);
      cards::set(c, 2, theme::ink(theme::Ink::ACCENT_MAGENTA), "%s", o.fact);
      cards::draw(matrix, c, e - kShowMs, 2, /*backdrop=*/true);
    }
  }

 private:
  void blit(Adafruit_Protomatter& m, uint16_t gain, uint32_t now_ms) {
    using namespace deep_sky_detail;
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        uint16_t px = m_img[y][x];
        if (px == 0) continue;
        if (gain < 256) {
          const uint16_t r = static_cast<uint16_t>(((px >> 11) * gain) >> 8);
          const uint16_t g = static_cast<uint16_t>((((px >> 5) & 0x3F) * gain) >> 8);
          const uint16_t b = static_cast<uint16_t>(((px & 0x1F) * gain) >> 8);
          px = static_cast<uint16_t>((r << 11) | (g << 5) | b);
        }
        m.drawPixel(x, kTop + y, px);
      }
    }
    // Twinkle: 4 stars from the hash field flick on/off every ~0.4 s.
    const uint32_t tick = now_ms / 400u;
    for (uint8_t k = 0; k < 4; ++k) {
      const int16_t x = hash8(static_cast<int32_t>(tick), k, 91) % PANEL_WIDTH;
      const int16_t y = hash8(k, static_cast<int32_t>(tick), 37) % kH;
      if (m_img[y][x] == 0) m.drawPixel(x, kTop + y, gain < 256 ? 0x2104 : 0x8410);
    }
  }

  void paint(uint8_t idx) {
    using namespace deep_sky_detail;
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        // Sparse background stars, every object.
        const uint8_t s = hash8(x, y, 1234u + idx);
        m_img[y][x] = s > 250 ? pack(s - 150, s - 150, s - 130) : 0;
      }
    }
    switch (idx) {
      case 0: paint_orion();     break;
      case 1: paint_andromeda(); break;
      case 2: paint_pillars();   break;
      case 3: paint_ring();      break;
      case 4: paint_crab();      break;
      default: paint_pleiades(); break;
    }
    m_painted = idx;
  }

  void put(int16_t x, int16_t y, uint16_t px) {
    if (x >= 0 && x < PANEL_WIDTH && y >= 0 && y < deep_sky_detail::kH) m_img[y][x] = px;
  }

  void paint_orion() {
    using namespace deep_sky_detail;
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        const int32_t dx = x - 30, dy = (y - 12) * 2;
        const int32_t f = falloff(dx * dx + dy * dy, 26 * 26);
        if (f == 0) continue;
        const int32_t n = noise(x, y, 11);
        const int32_t v = (f * (n + 60)) >> 8;
        const int32_t core = falloff(dx * dx + dy * dy, 8 * 8);
        m_img[y][x] = pack(v + core / 4, (v * 2) / 5 + core / 2, (v * 3) / 5 + core / 2);
      }
    }
    const int16_t trap[4][2] = { {29, 11}, {31, 10}, {32, 12}, {30, 13} };
    for (const auto& p : trap) put(p[0], p[1], 0xFFFF);
  }

  void paint_andromeda() {
    using namespace deep_sky_detail;
    // Galaxy tilted ~25°: rotate by (cos, sin) ≈ (0.906, 0.423) in Q8.
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        const int32_t dx = x - 32, dy = y - 12;
        const int32_t u = (dx * 232 + dy * 108) >> 8;
        const int32_t w = (-dx * 108 + dy * 232) >> 8;
        const int32_t d2 = u * u + w * w * 20;
        const int32_t f = falloff(d2, 29 * 29);
        if (f == 0) continue;
        const int32_t core = falloff(u * u + w * w * 4, 6 * 6);
        int32_t v = (f * (noise(x, y, 23) / 3 + 120)) >> 8;
        if (w == 1 && u > -18 && u < 22) v /= 3;  // dust lane
        m_img[y][x] = pack(v * 3 / 4 + core, v * 3 / 4 + core * 3 / 4, v + core / 2);
      }
    }
    put(38, 7, 0xC618);   // M32
    put(23, 17, 0x8410);  // M110
  }

  void paint_pillars() {
    using namespace deep_sky_detail;
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        const int32_t n = noise(x, y, 41);
        m_img[y][x] = pack(n / 6, 40 + n / 3, 50 + n / 3);  // teal glow
      }
    }
    struct Col { int16_t cx, top, w; };
    const Col cols[3] = { {18, 2, 6}, {34, 9, 5}, {47, 14, 4} };
    for (const Col& c : cols) {
      for (int16_t y = c.top; y < kH; ++y) {
        const int16_t half = static_cast<int16_t>(1 + ((y - c.top) * c.w) / (kH - c.top + 1));
        for (int16_t x = c.cx - half; x <= c.cx + half; ++x) {
          const bool edge = (x == c.cx - half || x == c.cx + half || y == c.top);
          const int32_t n = noise(x, y, 77);
          put(x, y, edge ? pack(220, 130 + n / 4, 60) : pack(70 + n / 5, 35 + n / 8, 15));
        }
      }
    }
  }

  void paint_ring() {
    using namespace deep_sky_detail;
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        const int32_t dx = x - 32, dy = (y - 12) * 5 / 4;
        const int32_t d2 = dx * dx + dy * dy;
        if (d2 > 11 * 11) continue;
        const int32_t inner = falloff(d2, 5 * 5);
        const int32_t ring = d2 >= 5 * 5 ? falloff((d2 - 64) < 0 ? -(d2 - 64) * 2 : (d2 - 64), 60) : 0;
        const int32_t rim  = d2 >= 8 * 8 ? falloff(d2 - 81 < 0 ? 81 - d2 : d2 - 81, 50) : 0;
        m_img[y][x] = pack(ring / 3 + rim, ring * 2 / 3 + inner / 3, inner / 2 + ring / 2 + 30);
      }
    }
    put(32, 12, 0xFFFF);
  }

  void paint_crab() {
    using namespace deep_sky_detail;
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        const int32_t dx = x - 32, dy = (y - 12) * 2;
        const int32_t n = noise(x, y, 59);
        const int32_t f = falloff(dx * dx + dy * dy, (18 + n / 32) * (18 + n / 32));
        if (f == 0) continue;
        const bool filament = (n & 0x1F) > 22;
        const int32_t glow = falloff(dx * dx + dy * dy, 9 * 9);
        m_img[y][x] = filament ? pack(f, f / 2, f / 6)
                               : pack(glow / 3, glow / 2, glow * 3 / 4 + f / 6);
      }
    }
  }

  void paint_pleiades() {
    using namespace deep_sky_detail;
    const int16_t stars[7][2] = { {20, 8}, {27, 6}, {31, 10}, {36, 7}, {41, 12}, {33, 15}, {26, 14} };
    for (int16_t y = 0; y < kH; ++y) {
      for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
        int32_t haze = 0;
        for (const auto& s : stars) {
          const int32_t dx = x - s[0], dy = y - s[1];
          haze += falloff(dx * dx + dy * dy, 36) / 3;
        }
        if (haze > 0) m_img[y][x] = pack(haze / 4, haze / 3, haze);
      }
    }
    for (const auto& s : stars) {
      put(s[0], s[1], 0xDF1F);
      put(s[0] + 1, s[1], 0x5AFF);
      put(s[0] - 1, s[1], 0x5AFF);
      put(s[0], s[1] + 1, 0x5AFF);
      put(s[0], s[1] - 1, 0x5AFF);
    }
  }

  uint16_t m_img[deep_sky_detail::kH][PANEL_WIDTH] = {};
  uint8_t  m_idx      = 0;
  uint8_t  m_painted  = 0xFF;
  uint32_t m_start_ms = 0;
};

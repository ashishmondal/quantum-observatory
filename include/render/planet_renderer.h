// Procedural planet renderer (phase 7.7a).
//
// Header-only utility that draws a believable-looking sphere onto an
// Adafruit_Protomatter framebuffer at arbitrary integer center + radius
// (1..16 px). The look is fully derived from a string seed (FNV-1a 32-bit
// hash of the planet name) so the same name always produces the same
// planet across reboots and theme switches — but a different name
// produces a visibly different one.
//
// Designed for the exoplanet_count scene's intro animation:
//
//     DOT (r=1)  →  ZOOM (r=1..16)  →  SLIDE (r=16..12 + park-right)
//
// then SETTLED rotation in place driven by the `time_phase` argument
// (typically `(now_ms >> 7) & 0xFF`, giving one full turn every ~30 s).
//
// Reusable by future visibility scenes (saturn / mars / venus_phase /
// etc.) — drop in, seed it, render at whatever (cx, cy, r) the scene
// composition wants.
//
// Math discipline (NFR-1.3): per-frame render path is integer-only.
// One-time palette synthesis at seed() uses small fixed-point HSV→RGB
// math; no <math.h>, no float.
//
// Memory: one ProceduralPlanet instance is ~32 B (8 RGB565 palette
// entries + 4 B cached hash + a handful of seed-derived bytes). No
// heap. No persistent sphere mask — the mask is recomputed per frame
// inline with the colour lookup, which is ≤ 1024 pixels × a few integer
// ops at the largest radius (well under the per-frame budget on
// RP2040 even with several planets on screen at once).
//
// Light source is baked at top-left (−1, −1, +1) normalised — keeps
// the rendering deterministic across themes and matches the visual
// intuition the rest of the bitmap art uses.

#pragma once

#include <stdint.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

namespace planet {

// ── Archetypes — selected by 3 bits of the name hash ─────────────────
enum class Archetype : uint8_t {
  ROCKY      = 0,  // muddy brown/tan, noisy surface (Mars-ish)
  ICY        = 1,  // pale blue/white with sparse darker speckles
  GAS_BANDED = 2,  // horizontal cream/tan bands (Jupiter-ish)
  GAS_STORMY = 3,  // banded + 1..3 oval storms
  OCEAN      = 4,  // deep blue with green/white "continents"
  LAVA       = 5,  // black/red with glowing fractured veins
  DESERT     = 6,  // pale yellow/orange dunes
  RINGED     = 7,  // gas-banded with a thin equatorial ring (≥ r=10 only)
};

// One self-contained planet. Seed once with a name; render at any
// (cx, cy, r) thereafter.
class ProceduralPlanet {
 public:
  ProceduralPlanet() = default;

  // Seed (or re-seed) the planet from a name. Cheap (one hash, eight
  // palette syntheses) but not free — call only when the source name
  // actually changes. Same name in == same look out. NUL-terminated
  // input; tolerant of empty / nullptr (deterministic "anonymous"
  // fallback). Theme-agnostic on purpose — the planet's identity
  // doesn't shift with chrome.
  void seed(const char* name);

  // Re-render the planet centered at (cx, cy) with sphere radius r in
  // pixels (1..16). r=1 produces a single lit pixel (the DOT phase).
  // For r ≥ 2 the renderer paints a shaded sphere + surface detail
  // + (if RINGED and r ≥ 10) a thin equatorial ring.
  //
  // time_phase advances longitude rotation — pass a slow-moving byte
  // such as `(now_ms >> 7) & 0xFF` for ~30 s/turn. Animation cost is
  // purely a per-frame redraw; nothing is cached frame-to-frame.
  void render(Adafruit_Protomatter& matrix,
              int16_t cx, int16_t cy,
              uint8_t r,
              uint8_t time_phase) const;

  // Diagnostic: which archetype did this seed land on? (For the
  // optional procplanet_demo scene's HUD.)
  Archetype archetype() const { return m_archetype; }
  uint32_t  hash()      const { return m_hash; }

 private:
  uint32_t  m_hash       = 0;
  Archetype m_archetype  = Archetype::ROCKY;
  uint8_t   m_band_freq  = 4;   // surface-feature scale, 2..8
  uint8_t   m_storm_seed = 0;   // GAS_STORMY storm placement
  // 8-entry palette synthesised from base hue + archetype template.
  // [0] = darkest shadow, [7] = brightest highlight. Linear ramp in
  // value space so Lambert shading can index by `(shade >> 5) & 7`.
  uint16_t  m_palette[8] = {0};
};

// ── FNV-1a 32-bit hash ───────────────────────────────────────────────
inline uint32_t fnv1a(const char* s) {
  uint32_t h = 0x811c9dc5u;
  if (s == nullptr) return h;
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 0x01000193u;
  }
  return h;
}

// ── HSV → RGB565 (8-bit per channel intermediate, integer) ───────────
// h: 0..255 (full hue wheel), s,v: 0..255.
inline uint16_t hsv_to_rgb565(uint8_t h, uint8_t s, uint8_t v) {
  if (s == 0) {
    const uint8_t g = v;
    return static_cast<uint16_t>(((g & 0xF8) << 8) |
                                 ((g & 0xFC) << 3) |
                                  (g >> 3));
  }
  const uint8_t region = h / 43;          // 0..5
  const uint8_t rem    = (h - region * 43) * 6;  // 0..255 within region
  const uint8_t p = (uint16_t(v) * (255 - s)) >> 8;
  const uint8_t q = (uint16_t(v) * (255 - ((uint16_t(s) * rem) >> 8))) >> 8;
  const uint8_t t = (uint16_t(v) * (255 - ((uint16_t(s) * (255 - rem)) >> 8))) >> 8;
  uint8_t r8, g8, b8;
  switch (region) {
    case 0:  r8 = v; g8 = t; b8 = p; break;
    case 1:  r8 = q; g8 = v; b8 = p; break;
    case 2:  r8 = p; g8 = v; b8 = t; break;
    case 3:  r8 = p; g8 = q; b8 = v; break;
    case 4:  r8 = t; g8 = p; b8 = v; break;
    default: r8 = v; g8 = p; b8 = q; break;
  }
  return static_cast<uint16_t>(((r8 & 0xF8) << 8) |
                               ((g8 & 0xFC) << 3) |
                                (b8 >> 3));
}

// ── Integer sqrt (small inputs only, ≤ 16² = 256) ────────────────────
inline uint8_t isqrt_u16(uint16_t n) {
  uint16_t r = 0, b = 1u << 14;
  while (b > n) b >>= 2;
  while (b > 0) {
    if (n >= r + b) { n -= r + b; r = (r >> 1) + b; }
    else            { r >>= 1; }
    b >>= 2;
  }
  return static_cast<uint8_t>(r);
}

// ── Cheap 2D hash for value-noise on (lat, lon) cells ────────────────
inline uint8_t cell_noise(uint32_t seed, int16_t a, int16_t b) {
  uint32_t h = seed ^ (uint32_t(uint16_t(a)) * 0x9E3779B1u)
                    ^ (uint32_t(uint16_t(b)) * 0x85EBCA77u);
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
  return static_cast<uint8_t>(h);
}

// ─────────────────────────────────────────────────────────────────────
// Implementation
// ─────────────────────────────────────────────────────────────────────

inline void ProceduralPlanet::seed(const char* name) {
  m_hash = fnv1a(name);

  // Archetype: low 3 bits — uniform-ish distribution across the 8.
  m_archetype = static_cast<Archetype>(m_hash & 0x7);

  // Base hue: 8 bits from the middle of the hash. Each archetype
  // pulls the hue toward a believable band, so a "ROCKY" planet
  // doesn't come out neon pink even with an unlucky seed.
  const uint8_t hue_raw = static_cast<uint8_t>((m_hash >> 8) & 0xFF);
  uint8_t hue_centre = 0;
  uint8_t hue_spread = 16;
  switch (m_archetype) {
    case Archetype::ROCKY:      hue_centre =  16; hue_spread = 12; break; // warm orange-brown
    case Archetype::ICY:        hue_centre = 150; hue_spread = 20; break; // pale blue
    case Archetype::GAS_BANDED: hue_centre =  20; hue_spread = 24; break; // cream/tan
    case Archetype::GAS_STORMY: hue_centre =  10; hue_spread = 28; break; // ruddy
    case Archetype::OCEAN:      hue_centre = 160; hue_spread = 16; break; // ocean blue
    case Archetype::LAVA:       hue_centre =   2; hue_spread =  8; break; // red-hot
    case Archetype::DESERT:     hue_centre =  32; hue_spread = 12; break; // sandy yellow
    case Archetype::RINGED:     hue_centre =  24; hue_spread = 20; break; // tan
  }
  const uint8_t base_hue =
      static_cast<uint8_t>(hue_centre + (int8_t(hue_raw) >> 4) %
                           int(hue_spread + 1));

  // Surface-feature scale: 2..8 bands across the visible hemisphere.
  m_band_freq  = 2 + static_cast<uint8_t>((m_hash >> 16) & 0x7);
  m_storm_seed = static_cast<uint8_t>((m_hash >> 24) & 0xFF);

  // Build the 8-entry value ramp. Archetype picks the (sat, vmin, vmax)
  // envelope; the hue is mostly constant across the ramp but a small
  // archetype-specific hue-shift in the highlights gives lava/ocean
  // their characteristic two-tone look without a second palette pass.
  //
  // V-range discipline: keep vmax-vmin ≥ ~150 so the 8-entry ramp
  // spans enough Lambert shading steps to read as a SHADED SPHERE
  // rather than a flat disc on a 64×32 RGB matrix (which compresses
  // anything ≥ ~50% V into "looks white"). Keep S ≥ ~140 for the
  // same reason — low-sat pale palettes look uniformly white at
  // these pixel counts even when the hue is technically present.
  uint8_t sat = 200, vmin = 20, vmax = 240;
  int8_t  highlight_hue_shift = 0;
  switch (m_archetype) {
    case Archetype::ROCKY:      sat = 200; vmin =  20; vmax = 220; break;
    case Archetype::ICY:        sat = 140; vmin =  40; vmax = 240; break;
    case Archetype::GAS_BANDED: sat = 170; vmin =  40; vmax = 230; break;
    case Archetype::GAS_STORMY: sat = 200; vmin =  40; vmax = 220; break;
    case Archetype::OCEAN:      sat = 220; vmin =  20; vmax = 210;
                                highlight_hue_shift = -40; /* greens at peaks */ break;
    case Archetype::LAVA:       sat = 255; vmin =  10; vmax = 255;
                                highlight_hue_shift = +6;  /* yellow-hot peaks */ break;
    case Archetype::DESERT:     sat = 210; vmin =  40; vmax = 230; break;
    case Archetype::RINGED:     sat = 180; vmin =  40; vmax = 230; break;
  }
  for (int i = 0; i < 8; ++i) {
    const uint8_t v = static_cast<uint8_t>(
        vmin + (uint16_t(vmax - vmin) * i) / 7);
    const uint8_t h = static_cast<uint8_t>(
        int(base_hue) + (highlight_hue_shift * i) / 7);
    m_palette[i] = hsv_to_rgb565(h, sat, v);
  }
}

inline void ProceduralPlanet::render(Adafruit_Protomatter& matrix,
                                     int16_t cx, int16_t cy,
                                     uint8_t r,
                                     uint8_t time_phase) const {
  if (r == 0) return;
  if (r == 1) {
    matrix.drawPixel(cx, cy, m_palette[6]);
    return;
  }

  const int16_t r_i  = static_cast<int16_t>(r);
  const uint16_t r2  = uint16_t(r_i) * uint16_t(r_i);

  // RINGED: paint the back half of the ring (behind the planet) first.
  // Only meaningful when the planet is large enough that the ring
  // reads as a thin line rather than a smudge.
  const bool draw_ring = (m_archetype == Archetype::RINGED) && (r >= 10);
  if (draw_ring) {
    const int16_t ring_rx = static_cast<int16_t>(r + r / 2);  // ~1.5×r horiz
    const int16_t ring_ry = static_cast<int16_t>(r / 4);      // squashed
    for (int16_t x = -ring_rx; x <= ring_rx; ++x) {
      // Ellipse: (x/rx)² + (y/ry)² = 1  →  y = ry * sqrt(1 - (x/rx)²)
      const int32_t num = int32_t(ring_rx) * int32_t(ring_rx)
                        - int32_t(x) * int32_t(x);
      if (num < 0) continue;
      const int16_t y = static_cast<int16_t>(
          (int32_t(ring_ry) * isqrt_u16(static_cast<uint16_t>(num)))
          / ring_rx);
      // Back half = upper arc (Lambert-light from above means the far
      // side of the ring sits "above" the equator on a 64×32 screen).
      matrix.drawPixel(cx + x, cy - y, m_palette[3]);
    }
  }

  // Sphere body — per-pixel mask + surface lookup.
  for (int16_t dy = -r_i; dy <= r_i; ++dy) {
    for (int16_t dx = -r_i; dx <= r_i; ++dx) {
      const uint16_t d2 = uint16_t(dx * dx + dy * dy);
      if (d2 > r2) continue;
      // z = sqrt(r² - x² - y²) — the front-facing depth at this pixel.
      const uint8_t z = isqrt_u16(static_cast<uint16_t>(r2 - d2));

      // Lambert shade with light ≈ (-1, -1, +1)/√3. Range collapse:
      // raw = -dx - dy + z, max ≈ r * √3 ≈ 1.732r.
      // Map raw → 0..255, clamped.
      int32_t raw = int32_t(z) - int32_t(dx) - int32_t(dy);
      if (raw < 0) raw = 0;
      const int32_t denom = (int32_t(r_i) * 173) / 100;  // r·√3 ≈ 1.73r
      uint8_t shade = denom > 0
          ? static_cast<uint8_t>((raw * 255) / denom)
          : 128;

      // Surface colour — varies by archetype. lat/lon proxies:
      // lat_idx ≈ dy (latitude band index, -r..r)
      // lon_idx ≈ dx + time_phase rotation (cheap planar proxy; good
      //           enough at this resolution since the silhouette
      //           dominates the perceived "spin")
      const int16_t lat_idx = dy;
      const int16_t lon_idx = static_cast<int16_t>(dx + int16_t(time_phase));

      uint8_t feature = 128;  // 0..255, modulates the value ramp
      switch (m_archetype) {
        case Archetype::GAS_BANDED:
        case Archetype::RINGED: {
          // Horizontal bands modulated by latitude.
          const int16_t band =
              (lat_idx * int16_t(m_band_freq)) + int16_t(m_hash & 0x1F);
          // Triangle wave from `band` mod 8 → 0..255.
          const uint8_t t = static_cast<uint8_t>(band & 0x7);
          feature = static_cast<uint8_t>(t < 4 ? (t * 64) : ((7 - t) * 64));
          break;
        }
        case Archetype::GAS_STORMY: {
          // Banded base + 2 storm cells from the storm seed.
          const int16_t band =
              (lat_idx * int16_t(m_band_freq)) + int16_t(m_hash & 0x1F);
          const uint8_t t = static_cast<uint8_t>(band & 0x7);
          feature = static_cast<uint8_t>(t < 4 ? (t * 64) : ((7 - t) * 64));
          // Storm centres (ovals): hashed (lat, lon) anchors.
          const int16_t sa_lat = int8_t((m_storm_seed >> 0) & 0xF) - 8;
          const int16_t sa_lon = int8_t((m_storm_seed >> 4) & 0xF) * 2 - 16;
          const int16_t dlat   = lat_idx - sa_lat;
          const int16_t dlon   = lon_idx - sa_lon;
          if ((dlat * dlat) + (dlon * dlon) / 4 < r) {
            feature = 220;  // bright storm spot
          }
          break;
        }
        case Archetype::ROCKY:
        case Archetype::DESERT: {
          // 2-octave value noise — chunky terrain.
          const uint8_t n1 = cell_noise(m_hash,
                                        lat_idx >> 1, lon_idx >> 1);
          const uint8_t n2 = cell_noise(m_hash ^ 0xA5A5A5A5u,
                                        lat_idx, lon_idx);
          feature = static_cast<uint8_t>((uint16_t(n1) + n2) >> 1);
          break;
        }
        case Archetype::OCEAN: {
          // Mostly dark; occasional bright continent splotches.
          const uint8_t n = cell_noise(m_hash, lat_idx >> 1, lon_idx >> 1);
          feature = n > 200 ? 230 : 40;
          break;
        }
        case Archetype::ICY: {
          // Bright base with rare dark specks.
          const uint8_t n = cell_noise(m_hash, lat_idx, lon_idx >> 1);
          feature = n < 32 ? 70 : 220;
          break;
        }
        case Archetype::LAVA: {
          // Black with glowing fractured veins; veins shift slowly.
          const uint8_t n = cell_noise(m_hash ^ (uint32_t(time_phase) << 8),
                                       lat_idx, lon_idx);
          feature = (n > 220) ? 240 : 20;
          break;
        }
      }

      // Combine Lambert shade with feature value, then index into the
      // 8-entry palette. shade dominates silhouette; feature gives
      // texture within a band.
      const uint16_t combined = (uint16_t(shade) * 3 + uint16_t(feature)) >> 2;
      const uint8_t pal_idx = static_cast<uint8_t>(combined >> 5);
      matrix.drawPixel(cx + dx, cy + dy, m_palette[pal_idx & 0x7]);
    }
  }

  // RINGED: front half of the ring (lower arc) painted last, on top
  // of the body, so it occludes the southern hemisphere as expected.
  if (draw_ring) {
    const int16_t ring_rx = static_cast<int16_t>(r + r / 2);
    const int16_t ring_ry = static_cast<int16_t>(r / 4);
    for (int16_t x = -ring_rx; x <= ring_rx; ++x) {
      const int32_t num = int32_t(ring_rx) * int32_t(ring_rx)
                        - int32_t(x) * int32_t(x);
      if (num < 0) continue;
      const int16_t y = static_cast<int16_t>(
          (int32_t(ring_ry) * isqrt_u16(static_cast<uint16_t>(num)))
          / ring_rx);
      matrix.drawPixel(cx + x, cy + y, m_palette[5]);
    }
  }
}

}  // namespace planet

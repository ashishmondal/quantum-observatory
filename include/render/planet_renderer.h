// Procedural planet renderer (phase 7.7b, v2).
//
// Header-only utility that draws a believable-looking sphere onto an
// Adafruit_Protomatter framebuffer at arbitrary integer center +
// radius (1..30 px), with three quality tiers selected automatically
// from the requested radius. Designed to be reusable for both
// "random exoplanet" (string seed → deterministic look) and
// "specific solar-system body" (named preset, hand-tuned to be
// recognisable) consumers.
//
// ── Two ways to invoke ──────────────────────────────────────────────
//
//   ProceduralPlanet p;
//   p.seed("kepler-22b");        // hash-derived archetype + palette
//   p.seed("jupiter");           // matches kSolarPresets[] — loads
//                                 //   hand-tuned Jupiter params (GAS_STORMY
//                                 //   + GRS at lat=-22° + correct hue)
//   p.configure(my_params);      // bypass hash entirely; caller owns
//                                 //   every knob (see planet::Params)
//
// Same name → same look across reboots, theme switches, and rebuilds.
// String compare is case-insensitive; presets stored lowercase.
//
// ── LOD tiers (automatic from `r`) ──────────────────────────────────
//
//   TINY  (r ≤ 3)   — 1..few lit pixels; archetype's brightest hue.
//                     No surface, no shading, no rotation; anything
//                     finer than this is invisible on a 64×32 panel.
//   SMALL (r 4..9)  — Lambert sphere + low-frequency bands + 1 storm
//                     spot + polar caps. Planar-dx rotation proxy
//                     (features slide rather than truly wrap — the
//                     eye can't tell at this scale).
//   LARGE (r ≥ 10) — True spherical (lat, lon) sampling via the
//                     shared Q8.8 sin LUT (fp::sin_q8/cos_q8). Real
//                     axial tilt rotation, anchored storms (visible
//                     only on the front hemisphere, drift slowly),
//                     multi-ring with Cassini-style gaps, polar caps,
//                     optional limb darkening / brightening.
//
// ── Animation ───────────────────────────────────────────────────────
//
//   render(..., time_phase) — pass `(now_ms >> 7) & 0xFF` for ~30 s/turn.
//   At LARGE LOD this drives true longitude spin (feature crosses the
//   limb on the correct side). Anchored storms drift at ~1/8 the
//   primary spin rate so a Great-Red-Spot-like feature creeps
//   relative to the bands. Rings get a per-frame brightness shimmer
//   so they don't look like a static line.
//
// ── Math discipline (NFR-1.3) ───────────────────────────────────────
//
//   Per-frame render path: integer-only. The Q8.8 sin LUT
//   (fp::sin_q8/cos_q8) and 8-bit isqrt do all the spherical work.
//   No <math.h>, no float in render().
//   One-time palette synthesis at seed()/configure() uses small
//   integer HSV→RGB; still no float.
//
// ── Memory ──────────────────────────────────────────────────────────
//
//   One ProceduralPlanet instance: ~64 B (8-entry RGB565 palette +
//   one Params struct + 4 B cached hash). No heap. Shared sin LUT
//   already in flash via fp::. Solar-system preset table is ~600 B
//   in flash, no RAM cost.

#pragma once

#include <stdint.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "astro/fixed_point.h"

namespace planet {

// ── Archetypes — family templates ──────────────────────────────────
//
// `seed()` from a hash picks one of these by `hash & 0x7`; presets
// just pick one explicitly to inform some defaults (e.g. RINGED
// implies ring_count ≥ 1). The actual look is fully governed by
// the Params struct — archetype is metadata for the diagnostic HUD.
enum class Archetype : uint8_t {
  ROCKY      = 0,  // muddy brown/tan, noisy surface (Mars-ish)
  ICY        = 1,  // pale blue/white with sparse darker speckles
  GAS_BANDED = 2,  // horizontal cream/tan bands (Jupiter-ish)
  GAS_STORMY = 3,  // banded + 1..3 oval storms
  OCEAN      = 4,  // deep blue with green/white "continents"
  LAVA       = 5,  // black/red with glowing fractured veins (sun-ish)
  DESERT     = 6,  // pale yellow/orange dunes
  RINGED     = 7,  // gas-banded with a thin equatorial ring system
};

// ── Params — full procedural-planet parameter set ──────────────────
//
// All fields are integer / fixed-point so this struct can live in
// flash (a `constexpr Params kJupiter = {...};` ROM table costs zero
// RAM until copied into a ProceduralPlanet instance via configure()).
//
// Fields are ordered for natural packing (largest groups together)
// and grouped by visual role.
struct Params {
  // Identity / palette
  Archetype  archetype          = Archetype::ROCKY;
  uint8_t    base_hue           = 16;     // 0..255 (full hue wheel)
  uint8_t    sat                = 200;    // 0..255
  uint8_t    vmin               = 20;     // 0..255 — dark end of shading ramp
  uint8_t    vmax               = 230;    // 0..255 — bright end; keep vmax-vmin ≥ 150
  int8_t     highlight_hue_shift = 0;     // hue rotation toward bright end (lava/ocean)

  // Surface bands (horizontal latitude bands)
  uint8_t    band_freq          = 4;      // 0 = none, else bands per hemisphere
  uint8_t    band_phase         = 0;      // band offset (0..7)
  uint8_t    band_contrast      = 200;    // 0..255 — how strongly bands modulate

  // Surface noise (cratering / continents / clouds)
  uint8_t    noise_scale        = 1;      // cell size in pixels; 0 = noise off
  uint8_t    noise_amp          = 128;    // 0..255 modulation depth

  // Polar caps (bright caps near poles for icy/Mars look)
  uint8_t    polar_cap_extent   = 0;      // 0 = no cap, else 0..64 = fraction of |y'|/r above which cap kicks in (255-based)
  uint8_t    polar_cap_value    = 240;    // brightness 0..255 of cap pixels

  // Anchored spots (GRS, Neptune's Great Dark Spot, etc.)
  uint8_t    spot_count         = 0;      // 0..3
  int8_t     spot_lat[3]        = {0,0,0};   // signed deg (-90..+90)
  uint8_t    spot_lon[3]        = {0,0,0};   // 0..255 = full revolution
  uint8_t    spot_size[3]       = {0,0,0};   // angular size hint (0..64)
  uint8_t    spot_hue[3]        = {0,0,0};   // absolute hue
  uint8_t    spot_value[3]      = {0,0,0};   // 0..255 brightness for the spot

  // Rings (multi-ring with gaps)
  uint8_t    ring_count         = 0;      // 0..3
  uint8_t    ring_inner[3]      = {0,0,0};   // ×16 of planet radius (24 = 1.5×r)
  uint8_t    ring_outer[3]      = {0,0,0};   // ×16 of planet radius
  uint8_t    ring_brightness[3] = {0,0,0};   // palette index 0..7
  uint8_t    ring_squash        = 4;         // y-flatten ×16; 4 = ring_ry = r/4

  // Orientation + animation
  int8_t     axial_tilt_deg     = 0;      // -90..+90; positive = top-pole tipped away
  uint8_t    rotation_speed     = 16;     // multiplier on time_phase (Q4: 16 = 1.0×)
  uint8_t    storm_drift_rate   = 0;      // 0 = anchored to body; else /16 of spin

  // Limb effects (one or the other)
  uint8_t    limb_darken        = 0;      // 0..255 strength (Mars/Mercury)
  uint8_t    limb_brighten      = 0;      // 0..255 (sun corona)
};

class ProceduralPlanet {
 public:
  ProceduralPlanet() = default;

  // Seed (or re-seed) from a string. Cheap (one hash + palette pass)
  // but not free — call only when the source name actually changes.
  //
  // If `name` matches a known solar-system body (case-insensitive),
  // loads the hand-tuned preset from kSolarPresets[]. Otherwise
  // derives a deterministic Params from FNV-1a 32-bit hash of the
  // name. NULL / empty → deterministic "anonymous" fallback.
  void seed(const char* name);

  // Bypass hash and presets entirely — caller owns every knob.
  void configure(const Params& p);

  // Re-render the planet centered at (cx, cy) with sphere radius r
  // in pixels (1..30). r=1 produces a single lit pixel (the DOT
  // phase used by the exoplanet intro). LOD tier picked from r.
  //
  // time_phase advances longitude rotation — pass a slow-moving
  // byte such as `(now_ms >> 7) & 0xFF` for ~30 s/turn. Animation
  // cost is purely a per-frame redraw; nothing is cached
  // frame-to-frame.
  void render(Adafruit_Protomatter& matrix,
              int16_t cx, int16_t cy,
              uint8_t r,
              uint8_t time_phase) const;

  // Diagnostics.
  Archetype     archetype() const { return m_params.archetype; }
  uint32_t      hash()      const { return m_hash; }
  const Params& params()    const { return m_params; }

 private:
  void render_tiny (Adafruit_Protomatter& m, int16_t cx, int16_t cy, uint8_t r) const;
  void render_small(Adafruit_Protomatter& m, int16_t cx, int16_t cy, uint8_t r, uint8_t time_phase) const;
  void render_large(Adafruit_Protomatter& m, int16_t cx, int16_t cy, uint8_t r, uint8_t time_phase) const;

  void build_palette();
  void seed_from_hash();

  // 8-entry palette synthesised from Params. [0] = darkest shadow,
  // [7] = brightest highlight. Linear value ramp + small hue shift
  // for lava/ocean two-tone look.
  uint16_t m_palette[8] = {0};
  Params   m_params     = {};
  uint32_t m_hash       = 0;
};

// ─────────────────────────────────────────────────────────────────────
// Helpers (inline, header-only)
// ─────────────────────────────────────────────────────────────────────

// FNV-1a 32-bit hash.
inline uint32_t fnv1a(const char* s) {
  uint32_t h = 0x811c9dc5u;
  if (s == nullptr) return h;
  while (*s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 0x01000193u;
  }
  return h;
}

// Case-insensitive compare; returns 0 on match.
inline int planet_strcasecmp(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
    if (ca != cb) return static_cast<int>(ca) - static_cast<int>(cb);
    ++a; ++b;
  }
  return static_cast<int>(*a) - static_cast<int>(*b);
}

// HSV → RGB565, integer-only. h/s/v in 0..255.
inline uint16_t hsv_to_rgb565(uint8_t h, uint8_t s, uint8_t v) {
  if (s == 0) {
    const uint8_t g = v;
    return static_cast<uint16_t>(((g & 0xF8) << 8) |
                                 ((g & 0xFC) << 3) |
                                  (g >> 3));
  }
  const uint8_t region = h / 43;
  const uint8_t rem    = (h - region * 43) * 6;
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

// Integer sqrt (inputs ≤ ~900 for r=30).
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

// Cheap 2D value-noise hash on integer (a, b) cells.
inline uint8_t cell_noise(uint32_t seed, int16_t a, int16_t b) {
  uint32_t h = seed ^ (uint32_t(uint16_t(a)) * 0x9E3779B1u)
                    ^ (uint32_t(uint16_t(b)) * 0x85EBCA77u);
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15;
  return static_cast<uint8_t>(h);
}

// Convert signed degrees (-180..+180) to uint8 angle (0..255 = full turn).
inline uint8_t deg_to_byte_angle(int16_t deg) {
  // 1 byte = 360/256 = 1.40625 deg. byte = deg * 256 / 360.
  // Use deg * 182 / 256 ≈ deg / 1.40625 with rounding.
  int32_t b = (int32_t(deg) * 182 + 128) >> 8;
  return static_cast<uint8_t>(b);
}

// ─────────────────────────────────────────────────────────────────────
// Solar-system preset table (flash-resident)
// ─────────────────────────────────────────────────────────────────────
//
// Each entry: lowercase name + hand-tuned Params. Names must match
// what callers pass to seed() after lowercasing. Coverage: 8 planets
// + sun + moon + 5 major moons (Io / Europa / Ganymede / Callisto /
// Titan). Pluto kept as a dwarf-planet honorable mention.
//
// Tuning notes per body — see commit history for rationale beyond
// the inline comments.

struct PresetEntry {
  const char* name;
  Params      params;
};

inline constexpr PresetEntry kSolarPresets[] = {
  // MERCURY — grey, heavy cratering, no atmosphere → no bands.
  {"mercury", {
    /*archetype*/        Archetype::ROCKY,
    /*base_hue*/         28,   // warm grey-tan
    /*sat*/              60,   // very desaturated
    /*vmin*/             30,
    /*vmax*/             220,
    /*highlight_shift*/  0,
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        180,  // heavy mottling
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    /*spot_lat*/         {0,0,0},
    /*spot_lon*/         {0,0,0},
    /*spot_size*/        {0,0,0},
    /*spot_hue*/         {0,0,0},
    /*spot_value*/       {0,0,0},
    /*ring_count*/       0,
    /*ring_inner*/       {0,0,0},
    /*ring_outer*/       {0,0,0},
    /*ring_brightness*/  {0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   8,    // slow rotator
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      80,
    /*limb_brighten*/    0,
  }},
  // VENUS — uniform cream-yellow haze, no surface detail visible.
  {"venus", {
    Archetype::GAS_BANDED, 30, 180, 90, 240, 0,
    /*band_freq*/        2,
    /*band_phase*/       0,
    /*band_contrast*/    40,    // very faint bands
    /*noise_scale*/      0,
    /*noise_amp*/        0,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   16,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      120,   // thick atmosphere
    /*limb_brighten*/    0,
  }},
  // EARTH — deep ocean blue with bright green continents + small polar
  // caps. The renderer's OCEAN archetype branch reads noise_amp as the
  // continent fraction threshold (255 = no land, 0 = all land) and
  // uses noise_scale as the cell size for chunky continent shapes;
  // ocean pixels paint from palette[0..3] (blue ramp) and land pixels
  // from palette[4..7] (green via highlight_hue_shift=-40).
  {"earth", {
    Archetype::OCEAN, 160, 230, 15, 230, -40,  // blue → green ramp
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      3,     // chunky 3-px continent cells
    /*noise_amp*/        140,   // OCEAN: continent threshold (lower = more land)
    /*polar_cap_extent*/ 40,
    /*polar_cap_value*/  240,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   23,
    /*rotation_speed*/   16,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      40,
    /*limb_brighten*/    0,
  }},
  // MARS — rust red, prominent N+S white polar caps, mild tilt.
  {"mars", {
    Archetype::ROCKY, 14, 220, 30, 230, 0,
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        160,
    /*polar_cap_extent*/ 70,    // BIG caps
    /*polar_cap_value*/  240,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   25,
    /*rotation_speed*/   16,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      80,
    /*limb_brighten*/    0,
  }},
  // JUPITER — cream/tan bands at correct frequency + anchored GRS.
  {"jupiter", {
    Archetype::GAS_STORMY, 22, 200, 40, 230, 6,
    /*band_freq*/        6,     // many narrow bands
    /*band_phase*/       0,
    /*band_contrast*/    220,
    /*noise_scale*/      0,     // no cratering; gas giant
    /*noise_amp*/        0,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       1,
    /*spot_lat*/         {-22, 0, 0},  // GRS lat ≈ −22°
    /*spot_lon*/         {128, 0, 0},
    /*spot_size*/        {14, 0, 0},
    /*spot_hue*/         {4, 0, 0},    // red
    /*spot_value*/       {180, 0, 0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   3,
    /*rotation_speed*/   24,    // fast rotator
    /*storm_drift_rate*/ 3,     // GRS drifts vs bands
    /*limb_darken*/      30,
    /*limb_brighten*/    0,
  }},
  // SATURN — pale tan bands, 3-ring system with Cassini gap, tilt 27°.
  {"saturn", {
    Archetype::RINGED, 28, 150, 50, 235, 0,
    /*band_freq*/        4,
    /*band_phase*/       0,
    /*band_contrast*/    150,
    /*noise_scale*/      0,
    /*noise_amp*/        0,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       3,
    // ring fractions ×16 of r: C=20..24, B=25..32 (brightest), A=33..40 (after Cassini gap)
    /*ring_inner*/       {20, 25, 33},
    /*ring_outer*/       {24, 32, 40},
    /*ring_brightness*/  {4, 7, 6},
    /*ring_squash*/      3,
    /*axial_tilt_deg*/   27,
    /*rotation_speed*/   24,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      30,
    /*limb_brighten*/    0,
  }},
  // URANUS — pale cyan, faint bands, dramatic 98° axial tilt, thin ring.
  {"uranus", {
    Archetype::ICY, 130, 130, 60, 230, 0,
    /*band_freq*/        3,
    /*band_phase*/       0,
    /*band_contrast*/    60,
    /*noise_scale*/      0,
    /*noise_amp*/        0,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       1,
    /*ring_inner*/       {28, 0, 0},
    /*ring_outer*/       {30, 0, 0},
    /*ring_brightness*/  {3, 0, 0},
    /*ring_squash*/      14,    // nearly edge-on from rotated view
    /*axial_tilt_deg*/   82,    // 98° wraps; we encode as 82 toward viewer
    /*rotation_speed*/   16,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      40,
    /*limb_brighten*/    0,
  }},
  // NEPTUNE — deep cobalt-blue banded gas giant + 1 dark storm spot.
  {"neptune", {
    Archetype::GAS_STORMY, 165, 230, 30, 200, 0,
    /*band_freq*/        4,
    /*band_phase*/       0,
    /*band_contrast*/    100,
    /*noise_scale*/      0,
    /*noise_amp*/        0,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       1,
    /*spot_lat*/         {-20, 0, 0},
    /*spot_lon*/         {96, 0, 0},
    /*spot_size*/        {10, 0, 0},
    /*spot_hue*/         {160, 0, 0},
    /*spot_value*/       {30, 0, 0},   // DARK spot (Neptune's GDS)
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   28,
    /*rotation_speed*/   18,
    /*storm_drift_rate*/ 5,
    /*limb_darken*/      40,
    /*limb_brighten*/    0,
  }},
  // PLUTO — small mottled tan-grey rocky dwarf.
  {"pluto", {
    Archetype::ROCKY, 20, 100, 30, 210, 0,
    /*band_freq*/        0, 0, 0,
    /*noise_scale*/      1,
    /*noise_amp*/        200,
    /*polar_cap_extent*/ 40,
    /*polar_cap_value*/  220,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   8,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      80,
    /*limb_brighten*/    0,
  }},
  // SUN — bright yellow-orange granulation, anti-limb-darken (corona-ish).
  {"sun", {
    Archetype::LAVA, 18, 240, 200, 255, 4,
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        80,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   8,
    /*storm_drift_rate*/ 6,     // surface granulation evolves
    /*limb_darken*/      0,
    /*limb_brighten*/    180,   // corona-ish bright limb
  }},
  // MOON — grey rocky with high-contrast mare patches.
  {"moon", {
    Archetype::ROCKY, 32, 30, 30, 220, 0,    // very low sat — true grey
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        220,   // strong mare contrast
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   4,     // tidally locked-ish (very slow)
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      60,
    /*limb_brighten*/    0,
  }},
  // IO — sulfur yellow with red/orange volcanic blotches.
  {"io", {
    Archetype::DESERT, 36, 230, 50, 240, -10,
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        200,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   12,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      40,
    /*limb_brighten*/    0,
  }},
  // EUROPA — smooth icy with hairline cracks (approximated via low-amp noise).
  {"europa", {
    Archetype::ICY, 24, 80, 80, 240, 0,
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        80,    // subtle cracks
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   8,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      30,
    /*limb_brighten*/    0,
  }},
  // GANYMEDE — grey-brown with strong contrast between bright + dark terrains.
  {"ganymede", {
    Archetype::ROCKY, 30, 80, 40, 230, 0,
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        180,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   8,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      60,
    /*limb_brighten*/    0,
  }},
  // CALLISTO — dark grey, heavy cratering — most cratered body in the system.
  {"callisto", {
    Archetype::ROCKY, 24, 60, 20, 180, 0,
    /*band_freq*/        0,
    /*band_phase*/       0,
    /*band_contrast*/    0,
    /*noise_scale*/      1,
    /*noise_amp*/        220,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   8,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      80,
    /*limb_brighten*/    0,
  }},
  // TITAN — uniform orange haze (no surface detail visible from space).
  {"titan", {
    Archetype::GAS_BANDED, 22, 200, 60, 220, 0,
    /*band_freq*/        2,
    /*band_phase*/       0,
    /*band_contrast*/    30,    // very faint
    /*noise_scale*/      0,
    /*noise_amp*/        0,
    /*polar_cap_extent*/ 0,
    /*polar_cap_value*/  0,
    /*spot_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    /*ring_count*/       0,
    {0,0,0},{0,0,0},{0,0,0},
    /*ring_squash*/      4,
    /*axial_tilt_deg*/   0,
    /*rotation_speed*/   8,
    /*storm_drift_rate*/ 0,
    /*limb_darken*/      140,   // thick atmosphere
    /*limb_brighten*/    0,
  }},
};

inline constexpr int kSolarPresetCount =
    sizeof(kSolarPresets) / sizeof(kSolarPresets[0]);

inline const Params* lookup_preset(const char* name) {
  if (name == nullptr || name[0] == '\0') return nullptr;
  for (int i = 0; i < kSolarPresetCount; ++i) {
    if (planet_strcasecmp(name, kSolarPresets[i].name) == 0) {
      return &kSolarPresets[i].params;
    }
  }
  return nullptr;
}

// ─────────────────────────────────────────────────────────────────────
// ProceduralPlanet implementation
// ─────────────────────────────────────────────────────────────────────

inline void ProceduralPlanet::configure(const Params& p) {
  m_params = p;
  build_palette();
}

inline void ProceduralPlanet::seed(const char* name) {
  m_hash = fnv1a(name);
  if (const Params* preset = lookup_preset(name)) {
    m_params = *preset;
    build_palette();
    return;
  }
  seed_from_hash();
  build_palette();
}

inline void ProceduralPlanet::build_palette() {
  for (int i = 0; i < 8; ++i) {
    const uint8_t v = static_cast<uint8_t>(
        m_params.vmin + (uint16_t(m_params.vmax - m_params.vmin) * i) / 7);
    const uint8_t h = static_cast<uint8_t>(
        int(m_params.base_hue) + (int(m_params.highlight_hue_shift) * i) / 7);
    m_palette[i] = hsv_to_rgb565(h, m_params.sat, v);
  }
}

inline void ProceduralPlanet::seed_from_hash() {
  // Archetype from low 3 bits.
  m_params = Params{};
  m_params.archetype = static_cast<Archetype>(m_hash & 0x7);

  // Base hue + per-archetype envelope (kept compatible with the v1
  // hash → planet mapping so existing exoplanet seeds remain
  // recognisable as the same body).
  const uint8_t hue_raw = static_cast<uint8_t>((m_hash >> 8) & 0xFF);
  uint8_t hue_centre = 0;
  uint8_t hue_spread = 16;
  uint8_t sat = 200, vmin = 20, vmax = 240;
  int8_t  highlight_hue_shift = 0;
  switch (m_params.archetype) {
    case Archetype::ROCKY:
      hue_centre = 16; hue_spread = 12;
      sat = 200; vmin = 20; vmax = 220; break;
    case Archetype::ICY:
      hue_centre = 150; hue_spread = 20;
      sat = 140; vmin = 40; vmax = 240; break;
    case Archetype::GAS_BANDED:
      hue_centre = 20; hue_spread = 24;
      sat = 170; vmin = 40; vmax = 230; break;
    case Archetype::GAS_STORMY:
      hue_centre = 10; hue_spread = 28;
      sat = 200; vmin = 40; vmax = 220; break;
    case Archetype::OCEAN:
      hue_centre = 160; hue_spread = 16;
      sat = 220; vmin = 20; vmax = 210;
      highlight_hue_shift = -40; break;
    case Archetype::LAVA:
      hue_centre = 2; hue_spread = 8;
      sat = 255; vmin = 10; vmax = 255;
      highlight_hue_shift = 6; break;
    case Archetype::DESERT:
      hue_centre = 32; hue_spread = 12;
      sat = 210; vmin = 40; vmax = 230; break;
    case Archetype::RINGED:
      hue_centre = 24; hue_spread = 20;
      sat = 180; vmin = 40; vmax = 230; break;
  }
  m_params.base_hue =
      static_cast<uint8_t>(hue_centre + (int8_t(hue_raw) >> 4) %
                           int(hue_spread + 1));
  m_params.sat                 = sat;
  m_params.vmin                = vmin;
  m_params.vmax                = vmax;
  m_params.highlight_hue_shift = highlight_hue_shift;

  // Bands + storms only for banded archetypes.
  m_params.band_freq     = 2 + static_cast<uint8_t>((m_hash >> 16) & 0x7);
  m_params.band_contrast = 200;
  switch (m_params.archetype) {
    case Archetype::GAS_BANDED:
    case Archetype::GAS_STORMY:
    case Archetype::RINGED:
      m_params.band_freq = 4 + static_cast<uint8_t>((m_hash >> 16) & 0x3);
      m_params.band_contrast = 220;
      break;
    default:
      m_params.band_freq = 0;     // non-banded archetypes get no bands
      m_params.band_contrast = 0;
      break;
  }

  // Surface noise — cratering / continents / lava.
  switch (m_params.archetype) {
    case Archetype::ROCKY:
    case Archetype::DESERT:
      m_params.noise_scale = 1; m_params.noise_amp = 160; break;
    case Archetype::OCEAN:
      // Bimodal: noise_amp is the continent threshold (0..255).
      // Larger noise_scale → chunkier continents.
      m_params.noise_scale = 3; m_params.noise_amp = 150; break;
    case Archetype::ICY:
      m_params.noise_scale = 1; m_params.noise_amp = 60;  break;
    case Archetype::LAVA:
      m_params.noise_scale = 1; m_params.noise_amp = 240; break;
    default:
      m_params.noise_scale = 0; m_params.noise_amp = 0;   break;
  }

  // Storms — only GAS_STORMY gets one anchored spot.
  if (m_params.archetype == Archetype::GAS_STORMY) {
    m_params.spot_count   = 1;
    m_params.spot_lat[0]  = static_cast<int8_t>((int8_t((m_hash >> 24) & 0xF) - 8) * 4);
    m_params.spot_lon[0]  = static_cast<uint8_t>((m_hash >> 20) & 0xFF);
    m_params.spot_size[0] = 10;
    m_params.spot_hue[0]  = static_cast<uint8_t>(m_params.base_hue + 8);
    m_params.spot_value[0] = 200;
    m_params.storm_drift_rate = 3;
  }

  // Rings — only the RINGED archetype, single ring.
  if (m_params.archetype == Archetype::RINGED) {
    m_params.ring_count        = 1;
    m_params.ring_inner[0]     = 22;
    m_params.ring_outer[0]     = 26;
    m_params.ring_brightness[0] = 5;
    m_params.ring_squash       = 4;
  }

  m_params.axial_tilt_deg = static_cast<int8_t>(
      (int8_t((m_hash >> 12) & 0xF) - 8) * 3);  // ±24°
  m_params.rotation_speed = 16;
  m_params.limb_darken    = 40;
}

// ─────────────────────────────────────────────────────────────────────
// Render — LOD dispatch
// ─────────────────────────────────────────────────────────────────────

inline void ProceduralPlanet::render(Adafruit_Protomatter& matrix,
                                     int16_t cx, int16_t cy,
                                     uint8_t r,
                                     uint8_t time_phase) const {
  if (r == 0) return;
  if (r <= 3)        render_tiny (matrix, cx, cy, r);
  else if (r <= 9)   render_small(matrix, cx, cy, r, time_phase);
  else               render_large(matrix, cx, cy, r, time_phase);
}

// ── TINY (r ≤ 3) ────────────────────────────────────────────────────
// One brightest-palette dot + a darker fill ring. No shading.
inline void ProceduralPlanet::render_tiny(Adafruit_Protomatter& matrix,
                                          int16_t cx, int16_t cy,
                                          uint8_t r) const {
  if (r == 1) {
    matrix.drawPixel(cx, cy, m_palette[6]);
    return;
  }
  const int16_t r_i = static_cast<int16_t>(r);
  const uint16_t r2 = uint16_t(r_i) * uint16_t(r_i);
  for (int16_t dy = -r_i; dy <= r_i; ++dy) {
    for (int16_t dx = -r_i; dx <= r_i; ++dx) {
      const uint16_t d2 = uint16_t(dx * dx + dy * dy);
      if (d2 > r2) continue;
      // Crude: brightest at top-left lit pixel, body otherwise.
      const bool highlight = (dx <= 0 && dy <= 0 && d2 <= (r2 >> 2));
      matrix.drawPixel(cx + dx, cy + dy,
                       m_palette[highlight ? 7 : 4]);
    }
  }
}

// ── SMALL (r 4..9) ──────────────────────────────────────────────────
// Lambert sphere + low-frequency bands + 1 storm (if any) + caps.
// Features sampled in screen-space (the planar-dx proxy) since at
// this size the eye can't resolve true spherical wrap-at-limb.
inline void ProceduralPlanet::render_small(Adafruit_Protomatter& matrix,
                                           int16_t cx, int16_t cy,
                                           uint8_t r,
                                           uint8_t time_phase) const {
  const int16_t r_i = static_cast<int16_t>(r);
  const uint16_t r2 = uint16_t(r_i) * uint16_t(r_i);
  const int32_t denom = (int32_t(r_i) * 173) / 100;
  const int16_t lon_drift = static_cast<int16_t>(
      (uint16_t(time_phase) * m_params.rotation_speed) >> 4);

  for (int16_t dy = -r_i; dy <= r_i; ++dy) {
    for (int16_t dx = -r_i; dx <= r_i; ++dx) {
      const uint16_t d2 = uint16_t(dx * dx + dy * dy);
      if (d2 > r2) continue;
      const uint8_t z = isqrt_u16(static_cast<uint16_t>(r2 - d2));

      // Lambert: light ≈ (-1, -1, +1)/√3.
      int32_t raw = int32_t(z) - int32_t(dx) - int32_t(dy);
      if (raw < 0) raw = 0;
      uint8_t shade = denom > 0
          ? static_cast<uint8_t>((raw * 255) / denom)
          : 128;

      // Feature = band + noise. lon_idx folds in the drift.
      const int16_t lon_idx = static_cast<int16_t>(dx + lon_drift);
      uint8_t feature = 128;
      if (m_params.band_freq > 0) {
        const int16_t band =
            (dy * int16_t(m_params.band_freq)) + int16_t(m_params.band_phase);
        const uint8_t t = static_cast<uint8_t>(band & 0x7);
        const uint8_t tri = static_cast<uint8_t>(t < 4 ? (t * 64) : ((7 - t) * 64));
        feature = static_cast<uint8_t>(
            (uint16_t(feature) * (255 - m_params.band_contrast)
             + uint16_t(tri) * m_params.band_contrast) >> 8);
      }
      if (m_params.noise_amp > 0) {
        const uint8_t n = cell_noise(m_hash, dy, lon_idx);
        // Blend noise into feature by amp.
        feature = static_cast<uint8_t>(
            (uint16_t(feature) * (255 - m_params.noise_amp)
             + uint16_t(n) * m_params.noise_amp) >> 8);
      }

      // Polar cap — proxy on |dy|/r.
      const uint16_t abs_dy_n = static_cast<uint16_t>(
          (uint16_t(dy < 0 ? -dy : dy) * 255) / r);
      if (m_params.polar_cap_extent > 0 &&
          abs_dy_n > uint16_t(255 - m_params.polar_cap_extent)) {
        feature = m_params.polar_cap_value;
      }

      // Storm spot — single, screen-space proxy.
      if (m_params.spot_count > 0) {
        const int16_t sa_lat = m_params.spot_lat[0] / 8;   // deg → ~px
        const int16_t sa_lon =
            static_cast<int16_t>(int8_t(m_params.spot_lon[0] + time_phase));
        const int16_t dlat = dy - sa_lat;
        const int16_t dlon = dx - (sa_lon * r_i / 64);
        if ((dlat * dlat) + (dlon * dlon) < r) {
          feature = m_params.spot_value[0];
        }
      }

      // Combine + lookup.
      const uint16_t combined = (uint16_t(shade) * 3 + uint16_t(feature)) >> 2;
      const uint8_t pal_idx = static_cast<uint8_t>(combined >> 5) & 0x7;
      matrix.drawPixel(cx + dx, cy + dy, m_palette[pal_idx]);
    }
  }
}

// ── LARGE (r ≥ 10) ──────────────────────────────────────────────────
// True spherical sampling: per-pixel reverse-project to body frame
// using fp::sin_q8/cos_q8 for axial tilt. Features (bands, spots,
// caps) live in body-frame (lat, lon) coordinates so they wrap at
// the visible limb on the correct side.
//
// Anchored spots use a dot-product visibility test (cheap, no asin
// needed): for each spot we precompute its body-frame unit vector
// once per render(), and per-pixel we dot it against the pixel's
// body-frame unit vector. cos(angle) > threshold AND spot is on the
// front face (sp_z > 0 in viewer frame) → pixel lies inside the spot.
//
// Math note: rotation around screen-x axis (the viewer's horizontal)
// by axial tilt. y' = y*cos - z*sin, z' = y*sin + z*cos, x' = x.
inline void ProceduralPlanet::render_large(Adafruit_Protomatter& matrix,
                                           int16_t cx, int16_t cy,
                                           uint8_t r,
                                           uint8_t time_phase) const {
  const int16_t r_i = static_cast<int16_t>(r);
  const int32_t r_sq = int32_t(r_i) * int32_t(r_i);
  const int32_t denom = (int32_t(r_i) * 173) / 100;

  // Axial tilt sin/cos (Q8.8).
  const uint8_t tilt_a = deg_to_byte_angle(m_params.axial_tilt_deg);
  const int16_t st = fp::sin_q8(tilt_a);  // ±256
  const int16_t ct = fp::cos_q8(tilt_a);  // ±256

  // Spin phase (byte-angle). rotation_speed is Q4 (16 = 1.0×).
  const uint8_t spin_a = static_cast<uint8_t>(
      (uint16_t(time_phase) * m_params.rotation_speed) >> 4);
  // Storm phase drifts relative to body. storm_drift_rate /16 of spin.
  const uint8_t storm_drift = static_cast<uint8_t>(
      (uint16_t(time_phase) * m_params.storm_drift_rate) >> 4);

  // Precompute body-frame unit vectors for each spot (× r so we
  // avoid normalising in the per-pixel test). Visible-iff sp_z > 0
  // after applying the body→viewer rotation (i.e. the tilt of the
  // spot's lat into screen y).
  //
  // Spot's lon as seen by viewer = stored lon + spin (body rotates
  // under viewer) − storm_drift (spot drifts on body).
  struct SpotCache {
    int32_t sx, sy, sz;       // ×256 (Q8.8) of unit vector, viewer frame
    uint8_t size;
    bool    visible;
    uint8_t hue;
    uint8_t value;
  } spots[3];
  for (int i = 0; i < m_params.spot_count; ++i) {
    const uint8_t lat_a = deg_to_byte_angle(m_params.spot_lat[i]);
    const uint8_t lon_a = static_cast<uint8_t>(
        m_params.spot_lon[i] + spin_a - storm_drift);
    // Body frame: x = cos(lat)*sin(lon), y = sin(lat), z = cos(lat)*cos(lon)
    const int16_t cl = fp::cos_q8(lat_a);
    const int16_t sl = fp::sin_q8(lat_a);
    const int16_t cn = fp::cos_q8(lon_a);
    const int16_t sn = fp::sin_q8(lon_a);
    // Each product Q8.8*Q8.8 >> 8 → Q8.8.
    const int32_t bx = (int32_t(cl) * int32_t(sn)) >> 8;
    const int32_t by =  int32_t(sl);
    const int32_t bz = (int32_t(cl) * int32_t(cn)) >> 8;
    // Apply viewer-frame tilt (rotate body Y/Z by tilt around X):
    //   yv = by*ct + bz*st     (note inverse direction vs viewer→body)
    //   zv = -by*st + bz*ct
    // (We're going body→viewer; sign of st flips vs the per-pixel
    // viewer→body rotation below.)
    const int32_t xv = bx;
    const int32_t yv = (by * ct + bz * st) >> 8;
    const int32_t zv = (-by * st + bz * ct) >> 8;
    spots[i].sx = xv;
    spots[i].sy = yv;
    spots[i].sz = zv;
    spots[i].size  = m_params.spot_size[i];
    spots[i].hue   = m_params.spot_hue[i];
    spots[i].value = m_params.spot_value[i];
    spots[i].visible = (zv > 0);
  }

  // Rings — first pass: back half (behind body). Render multi-ring
  // outline by walking each ring's x extent and finding the ellipse
  // y. Brightness modulated per-frame by cell_noise for shimmer.
  for (int i = 0; i < m_params.ring_count; ++i) {
    if (m_params.ring_outer[i] <= 16) continue;  // ring fits inside body
    const int16_t rx_outer = (int16_t(r) * m_params.ring_outer[i]) >> 4;
    const int16_t rx_inner = (int16_t(r) * m_params.ring_inner[i]) >> 4;
    const int16_t ry = (int16_t(r) * m_params.ring_squash) >> 4;
    for (int16_t x = -rx_outer; x <= rx_outer; ++x) {
      // Outer ellipse y.
      const int32_t no = int32_t(rx_outer) * int32_t(rx_outer)
                       - int32_t(x) * int32_t(x);
      if (no < 0) continue;
      const int16_t yo = static_cast<int16_t>(
          (int32_t(ry) * isqrt_u16(static_cast<uint16_t>(no))) / rx_outer);
      // Inner ellipse y (for gap).
      int16_t yi = 0;
      if (x >= -rx_inner && x <= rx_inner) {
        const int32_t ni = int32_t(rx_inner) * int32_t(rx_inner)
                         - int32_t(x) * int32_t(x);
        yi = static_cast<int16_t>(
            (int32_t(ry) * isqrt_u16(static_cast<uint16_t>(ni < 0 ? 0 : ni)))
            / (rx_inner == 0 ? 1 : rx_inner));
      }
      // Shimmer: ±1 palette step from cell_noise.
      const uint8_t shim = cell_noise(m_hash ^ 0xC0FFEEu,
                                      x, int16_t(time_phase >> 2));
      uint8_t pal_idx = m_params.ring_brightness[i];
      if (shim > 200 && pal_idx < 7) pal_idx++;
      else if (shim < 50 && pal_idx > 0) pal_idx--;
      const uint16_t col = m_palette[pal_idx & 0x7];
      // Back-half = upper arc (y < 0 in screen). Two pixels per x:
      // the outer ring edge and (if inside the ring band) interior.
      for (int16_t y = -yo; y <= -yi; ++y) {
        matrix.drawPixel(cx + x, cy + y, col);
      }
    }
  }

  // Sphere body.
  for (int16_t dy = -r_i; dy <= r_i; ++dy) {
    const int32_t dy2 = int32_t(dy) * int32_t(dy);
    for (int16_t dx = -r_i; dx <= r_i; ++dx) {
      const int32_t d2 = int32_t(dx) * int32_t(dx) + dy2;
      if (d2 > r_sq) continue;
      const uint8_t z = isqrt_u16(static_cast<uint16_t>(r_sq - d2));

      // Lambert in viewer frame (light dir baked at (-1,-1,+1)/√3).
      int32_t raw = int32_t(z) - int32_t(dx) - int32_t(dy);
      if (raw < 0) raw = 0;
      uint8_t shade = denom > 0
          ? static_cast<uint8_t>((raw * 255) / denom)
          : 128;

      // Limb darkening / brightening: factor by z/r.
      if (m_params.limb_darken > 0) {
        const uint16_t f = (uint16_t(z) * 255) / r;   // 0..255
        const uint16_t k =
            255 - ((uint16_t(m_params.limb_darken) * (255 - f)) >> 8);
        shade = static_cast<uint8_t>((uint16_t(shade) * k) >> 8);
      }
      if (m_params.limb_brighten > 0) {
        const uint16_t f = (uint16_t(z) * 255) / r;   // 0=limb, 255=center
        const uint16_t boost =
            (uint16_t(m_params.limb_brighten) * (255 - f)) >> 8;
        const uint16_t s = uint16_t(shade) + boost;
        shade = s > 255 ? 255 : static_cast<uint8_t>(s);
      }

      // Viewer→body rotation around screen-x axis by axial tilt:
      //   y' = dy*ct - z*st   (de-tilted body y; proxy for sin(lat))
      //   z' = dy*st + z*ct   (de-tilted body z; needed for body-frame x)
      const int32_t yp = (int32_t(dy) * ct - int32_t(z) * st) >> 8;
      const int32_t zp = (int32_t(dy) * st + int32_t(z) * ct) >> 8;

      // Spin: rotate (dx, zp) around body Y axis by spin_a. This gives
      // a body-frame "longitude index" that's anchored to the planet,
      // so continents stay put on the body and sweep through the view
      // as the planet rotates (rather than crawling sideways across).
      //   xs = dx*cos(spin) + zp*sin(spin)
      // (zs not used — we hash on (yp, xs).)
      const int16_t cs = fp::cos_q8(spin_a);
      const int16_t ss = fp::sin_q8(spin_a);
      const int32_t xs = (int32_t(dx) * cs + zp * ss) >> 8;

      uint8_t feature = 128;
      bool    is_land = false;   // OCEAN continent flag

      // Bands — based on body-frame y (proxy for sin(lat)).
      if (m_params.band_freq > 0) {
        const int32_t band =
            (yp * int32_t(m_params.band_freq) / r_i) + int32_t(m_params.band_phase);
        const uint8_t t = static_cast<uint8_t>(band & 0x7);
        const uint8_t tri = static_cast<uint8_t>(t < 4 ? (t * 64) : ((7 - t) * 64));
        feature = static_cast<uint8_t>(
            (uint16_t(feature) * (255 - m_params.band_contrast)
             + uint16_t(tri) * m_params.band_contrast) >> 8);
      }

      // Surface noise — sampled in body frame (yp, xs) so features
      // are anchored to the planet and sweep with rotation. Cell
      // size = noise_scale (clamped to 1).
      if (m_params.noise_amp > 0) {
        const int16_t scale = m_params.noise_scale ? m_params.noise_scale : 1;
        // 2-octave value-noise: coarse cells set the continent
        // silhouette, fine cells add a touch of jagged coastline.
        const uint8_t n_coarse = cell_noise(
            m_hash,
            static_cast<int16_t>(yp / scale),
            static_cast<int16_t>(xs / scale));
        const uint8_t n_fine = cell_noise(
            m_hash ^ 0xA5A5A5A5u,
            static_cast<int16_t>(yp / ((scale + 1) >> 1)),
            static_cast<int16_t>(xs / ((scale + 1) >> 1)));
        const uint8_t n = static_cast<uint8_t>(
            (uint16_t(n_coarse) * 3 + uint16_t(n_fine)) >> 2);

        if (m_params.archetype == Archetype::OCEAN) {
          // Bimodal: noise above threshold is LAND (palette upper
          // half = green via highlight_hue_shift); else OCEAN (lower
          // half = blue). noise_amp is the threshold (0..255).
          is_land = (n > m_params.noise_amp);
        } else {
          // Other archetypes: blend noise into feature continuously.
          feature = static_cast<uint8_t>(
              (uint16_t(feature) * (255 - m_params.noise_amp)
               + uint16_t(n) * m_params.noise_amp) >> 8);
        }
      }

      // Polar caps — based on body-frame |y'|/r.
      if (m_params.polar_cap_extent > 0) {
        const int32_t abs_yp = yp < 0 ? -yp : yp;
        const int32_t cap_thresh = int32_t(r_i)
            - (int32_t(r_i) * int32_t(m_params.polar_cap_extent)) / 255;
        if (abs_yp > cap_thresh) {
          feature  = m_params.polar_cap_value;
          is_land  = true;  // caps render from upper-half palette too
        }
      }

      // Combine + lookup. For OCEAN-bimodal pixels we force the
      // palette index into the upper (land/cap) or lower (ocean)
      // half and let Lambert shading pick which step within that
      // half — so continent shading still tracks the light, but
      // ocean pixels stay BLUE no matter how bright they're lit.
      uint8_t pal_idx;
      if (m_params.archetype == Archetype::OCEAN) {
        // Use a 2-bit shade within each half so we get 4 ocean steps
        // and 4 land steps. shade >> 6 = 0..3.
        const uint8_t shade2 = static_cast<uint8_t>(shade >> 6);
        pal_idx = is_land
            ? static_cast<uint8_t>(4u + shade2)
            : shade2;
      } else {
        const uint16_t combined = (uint16_t(shade) * 3 + uint16_t(feature)) >> 2;
        pal_idx = static_cast<uint8_t>(combined >> 5) & 0x7;
      }
      uint16_t col = m_palette[pal_idx & 0x7];

      // Spot test — dot product against precomputed viewer-frame
      // unit vector × r². Visible iff spot on front face AND angle
      // within size threshold.
      for (int i = 0; i < m_params.spot_count; ++i) {
        if (!spots[i].visible) continue;
        // Pixel viewer-frame unit vector × r = (dx, dy, z).
        // Spot vector × 256 = (sx, sy, sz). Dot / (r * 256) ≈ cos(angle).
        const int32_t dot = (int32_t(dx) * spots[i].sx
                           + int32_t(dy) * spots[i].sy
                           + int32_t(z)  * spots[i].sz);
        // Threshold: cos(angle) > (1 - size/64). dot > r*256*(1-size/64).
        const int32_t cos_thresh =
            int32_t(r_i) * 256 - (int32_t(r_i) * 256 * spots[i].size) / 64;
        if (dot > cos_thresh) {
          col = hsv_to_rgb565(spots[i].hue, m_params.sat, spots[i].value);
        }
      }

      matrix.drawPixel(cx + dx, cy + dy, col);
    }
  }

  // Rings — second pass: front half (in front of body).
  for (int i = 0; i < m_params.ring_count; ++i) {
    if (m_params.ring_outer[i] <= 16) continue;
    const int16_t rx_outer = (int16_t(r) * m_params.ring_outer[i]) >> 4;
    const int16_t rx_inner = (int16_t(r) * m_params.ring_inner[i]) >> 4;
    const int16_t ry = (int16_t(r) * m_params.ring_squash) >> 4;
    for (int16_t x = -rx_outer; x <= rx_outer; ++x) {
      const int32_t no = int32_t(rx_outer) * int32_t(rx_outer)
                       - int32_t(x) * int32_t(x);
      if (no < 0) continue;
      const int16_t yo = static_cast<int16_t>(
          (int32_t(ry) * isqrt_u16(static_cast<uint16_t>(no))) / rx_outer);
      int16_t yi = 0;
      if (x >= -rx_inner && x <= rx_inner) {
        const int32_t ni = int32_t(rx_inner) * int32_t(rx_inner)
                         - int32_t(x) * int32_t(x);
        yi = static_cast<int16_t>(
            (int32_t(ry) * isqrt_u16(static_cast<uint16_t>(ni < 0 ? 0 : ni)))
            / (rx_inner == 0 ? 1 : rx_inner));
      }
      const uint8_t shim = cell_noise(m_hash ^ 0xC0FFEEu,
                                      x, int16_t(time_phase >> 2));
      uint8_t pal_idx = m_params.ring_brightness[i];
      if (shim > 200 && pal_idx < 7) pal_idx++;
      else if (shim < 50 && pal_idx > 0) pal_idx--;
      const uint16_t col = m_palette[pal_idx & 0x7];
      for (int16_t y = yi; y <= yo; ++y) {
        matrix.drawPixel(cx + x, cy + y, col);
      }
    }
  }
}

}  // namespace planet

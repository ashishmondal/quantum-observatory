// See include/theme.h. T.2 skeleton: APOLLO_AMBER only, values mirror
// today's hardcoded literals across scenes/* and gfx_text.h so the
// T.3 refactor produces zero visual delta.

#include "theme.h"

#include <string.h>

#include <Arduino.h>           // millis()
#include <Adafruit_GFX.h>      // GFXfont
#include <Fonts/Picopixel.h>
#include <Fonts/Tiny3x3a2pt7b.h>

// HEADER-role fonts (FR-15.9 / THEME.md §5). Each header is
// `#pragma once` + `static const GFXfont`, so including them here
// — the single translation unit that binds them into the per-theme
// Def tables — keeps PROGMEM cost at one copy per font.
#include "fonts/press_start_2p_8pt7b.h"  // APOLLO header
#include "fonts/vt323_8pt7b.h"            // NOSTROMO header
#include "fonts/pixel_operator_8pt7b.h"   // VECTREX + BLADE_RUNNER + LCARS header (shared)

// BODY-role pixel fonts. All three ship with Adafruit_GFX so the only
// cost is the per-theme binding below (no PROGMEM bundling). Per
// THEME.md §5: Apollo+Vectrex → Picopixel, Nostromo → TomThumb (no
// descenders, thin grid for CRT feel), Blade Runner+LCARS → Org_01
// (5×6 sans with true lowercase — fits the HUD/LCARS sans look).
#include <Fonts/TomThumb.h>
#include <Fonts/Org_01.h>

// Arduino's <Arduino.h> (pulled in transitively) defines `bit(b)` as a
// macro, which collides with our local `bit(Hint)` helper below. Undef
// it here — we don't use the Arduino macro in this TU.
#ifdef bit
#undef bit
#endif

#include "backgrounds.h"        // BgType
#include "color_palette.h"
#include "fonts/digital_7__mono_14pt7b.h"

namespace theme {

namespace {

// Per-theme definition table. One Def per Id, indexed by the active id.
// Hints stored as a bitmask keyed by Hint enum value — `has()` shifts
// and ANDs.
struct Def {
  uint16_t       inks[static_cast<int>(Ink::COUNT)];
  const GFXfont* fonts[static_cast<int>(FontRole::COUNT)];
  uint32_t       hint_mask;        // bit i set <=> Hint(i) active
  const char*    bracket_open;
  const char*    bracket_close;
};

constexpr uint32_t bit(Hint h) {
  return 1u << static_cast<uint32_t>(h);
}

// APOLLO_AMBER — values mirror the existing hardcoded literals so that
// the T.3 scene refactor lands as a pure search-and-replace:
//   - chrome (gfx_text.h draw_clock_chrome defaults): white on black halo
//   - header (gfx_text.h draw_header defaults):       white on black halo
//   - body   (giant_clock_scene date strip):          deep amber 0xF940
//   - ghost  (giant_clock_scene LCD ghost):           dim grey 0x0841
//   - divider (giant_clock_scene divider hline):      dim warm green 0x0300
//   - giant_digits (giant_clock_scene live HH:MM):    white 0xFFFF
//   - status_* / label / value / safety: extracted in T.3b from the
//     constexpr literals previously duplicated across iss_pass,
//     jupiter_visibility, moon_phase, constellation_now, night, and
//     thermal_safe. Apollo values match the originals byte-for-byte
//     so refactoring those scenes produces zero visual delta.
constexpr Def kApollo = {
  /*inks=*/{
    /*CHROME         */ 0xFFFF,
    /*CHROME_HALO    */ 0x0000,
    /*HEADER         */ 0xFFFF,
    /*HEADER_HALO    */ 0x0000,
    /*HEADER_GLOW    */ 0x0000,   // Apollo doesn't NEON_OUTLINE; unused
    /*HEADER_DIM     */ 0x0000,   // generic dim — scenes with identity headers use STATUS_*_DIM
    /*BODY           */ 0xF940,   // deep amber, matches giant_clock date
    /*BODY_HALO      */ 0x0000,
    /*BODY_GLOW      */ 0x0000,   // unused under Apollo
    /*ACCENT         */ 0xFFFF,   // white pop (THEME.md §2.1)
    /*ACCENT_MAGENTA */ 0xF81F,   // magenta — ISS crew, constellation highlight
    /*ALERT          */ 0xF800,   // red — priority callouts
    /*GHOST          */ 0x0821,   // ~3% neutral grey LCD ghost (R=1,G=1,B=1) — just visible against black, white digits dominate
    /*DIVIDER        */ 0x0300,   // dim warm green, matches giant_clock hline
    /*GIANT_DIGITS   */ 0xFFFF,   // white, matches giant_clock live digits
    /*STATUS_OK      */ 0x07E0,   // green — ISS/JUP visible
    /*STATUS_OK_DIM  */ 0x0140,   // dim green — ISS [ISS] header pulse low
    /*STATUS_WARN    */ 0xFD20,   // amber — countdown, daylight, offline badge
    /*STATUS_WARN_DIM*/ 0x6A00,   // dim amber — JUP [JUP] header pulse low
    /*STATUS_INFO    */ 0x07FF,   // cyan — altitude / magnitude / IAU code
    /*STATUS_STALE   */ 0xC100,   // dim amber-red — "WAIT" no fresh data
    /*STATUS_DIM     */ 0x630C,   // dim grey — JUP below horizon
    /*LABEL          */ 0x31A6,   // ~20% white — moon ILL / AGE labels
    /*VALUE          */ 0xCE79,   // ~80% white — moon values, latin name
    /*SAFETY         */ 0x4000,   // deep red — night + thermal_safe (low LED current)
  },
  /*fonts=*/{
    // Ladder order: MICRO, BODY, HEADER, CLOCK (FR-15.9).
    /*MICRO */ &Tiny3x3a2pt7b,           // shared across every theme
    /*BODY  */ &Picopixel,               // Apollo data lines (matches existing scenes)
    /*HEADER*/ &PressStart2P8pt7b,       // APOLLO MOCR chunky 8x8 (FR-15.9, closes FR-4.1)
    /*CLOCK */ &digital_7__mono_14pt7b,  // shared across every theme
  },
  /*hint_mask=*/ bit(Hint::GIANT_DIGIT_GHOST),
  /*bracket_open=*/  "[",
  /*bracket_close=*/ "]",
};

// NOSTROMO_GREEN — 80s Alien CRT (THEME.md §2.2). Reuses Apollo's
// font bindings in T.5; the BODY/HEADER per-theme typography swap
// lands in T.6 once the new TTF roster is converted. Hints
// SCANLINES + CURSOR_BLOCK are *declared* here so theme::has() reads
// true; the actual layout primitives that consume them ship in T.7
// (per the PLAN.md T.7 promise: "Each adds at least one new layout
// hint primitive in gfx_text.h"). No visual loss in the meantime —
// the green-on-black ink swap alone is what makes T.5's exit
// criterion ("flips end-to-end inside one frame") readable from
// across the room.
//
// Ink choices: phosphor green 0x07E0 dominates (chrome / header /
// body / status_ok), dim green 0x0140 covers ghost / divider /
// dim variants, yellow 0xFFE0 carries warn + alert (matches the
// MU/TH/UR caution-stripe palette), pale green-white 0xDFFB pops as
// ACCENT. SAFETY stays a deep red even under green CRT — it's a
// hardware-safety signal, the Nostromo coolant-leak aesthetic
// happens to read it the same way.
constexpr Def kNostromo = {
  /*inks=*/{
    /*CHROME         */ 0x07E0,   // phosphor green
    /*CHROME_HALO    */ 0x0000,
    /*HEADER         */ 0x07E0,
    /*HEADER_HALO    */ 0x0000,
    /*HEADER_GLOW    */ 0x0000,   // no NEON_OUTLINE under Nostromo
    /*HEADER_DIM     */ 0x0140,   // pulse-low for generic headers
    /*BODY           */ 0x07E0,
    /*BODY_HALO      */ 0x0000,
    /*BODY_GLOW      */ 0x0000,
    /*ACCENT         */ 0xDFFB,   // pale green-white pop
    /*ACCENT_MAGENTA */ 0xFFE0,   // yellow caution-stripe (Alien CRT alt accent)
    /*ALERT          */ 0xFFE0,   // yellow alert per THEME.md §2.2
    /*GHOST          */ 0x0040,   // ~3% green LCD ghost — halved again so live phosphor digits pop
    /*DIVIDER        */ 0x0140,
    /*GIANT_DIGITS   */ 0x07E0,   // phosphor green giant clock
    /*STATUS_OK      */ 0x07E0,
    /*STATUS_OK_DIM  */ 0x0140,
    /*STATUS_WARN    */ 0xFFE0,   // yellow
    /*STATUS_WARN_DIM*/ 0x4200,   // dim yellow-olive
    /*STATUS_INFO    */ 0x07E0,   // monochrome CRT — info reads as plain phosphor
    /*STATUS_STALE   */ 0x2100,   // dim olive
    /*STATUS_DIM     */ 0x0140,
    /*LABEL          */ 0x0560,   // dim green label per THEME.md §2.2
    /*VALUE          */ 0x07E0,
    /*SAFETY         */ 0x4000,   // deep red — hardware-safety override, theme-agnostic
  },
  /*fonts=*/{
    // T.6 binds VT323 to HEADER per THEME.md §2.2. BODY is TomThumb
    // — 3×5, no descenders, thinner grid than Picopixel — to read
    // as a CRT terminal under green phosphor (THEME.md §5).
    /*MICRO */ &Tiny3x3a2pt7b,
    /*BODY  */ &TomThumb,
    /*HEADER*/ &VT323_Regular12pt7b,     // NOSTROMO CRT terminal feel (12pt — VT323 needs vertical room to hint cleanly; see fonts.toml)
    /*CLOCK */ &digital_7__mono_14pt7b,
  },
  /*hint_mask=*/ bit(Hint::CURSOR_BLOCK),
  /*bracket_open=*/  ">",
  /*bracket_close=*/ "_",
};

// VECTREX_NEON — Atari/Vectrex vector arcade (THEME.md §2.3).
// Identity rides on the cyan + magenta complementary pair plus the
// NEON_OUTLINE hint (halo drawn in glow color, not black) — that's
// what sells the vector-glow look on a 5-bit panel. Body stays
// Picopixel, header is the shared Pixel Operator.
constexpr Def kVectrex = {
  /*inks=*/{
    /*CHROME         */ 0x07FF,   // cyan (H=180°, V=100%)
    /*CHROME_HALO    */ 0x3807,   // ~25% V magenta halo — H=300° complement, V dropped 4× from 0xF81F so the halo reads as a dim glow shadow under the cyan text. (HSV: text V=100% → halo V≈25% gives clear luminance separation)
    /*HEADER         */ 0x07FF,   // cyan
    /*HEADER_HALO    */ 0x3807,   // ~25% V magenta — NEON_OUTLINE swap, well below text V
    /*HEADER_GLOW    */ 0x3807,
    /*HEADER_DIM     */ 0x0210,   // dim cyan
    /*BODY           */ 0x07FF,
    /*BODY_HALO      */ 0x3807,
    /*BODY_GLOW      */ 0x3807,
    /*ACCENT         */ 0xFFFF,   // white pop per THEME.md §2.3
    /*ACCENT_MAGENTA */ 0xF81F,
    /*ALERT          */ 0xF800,
    /*GHOST          */ 0x0041,   // ~3% cyan ghost — halved again so live cyan digits read as the foreground
    /*DIVIDER        */ 0x0210,
    /*GIANT_DIGITS   */ 0x07FF,
    /*STATUS_OK      */ 0x07FF,   // cyan reads as "OK" under Vectrex
    /*STATUS_OK_DIM  */ 0x0210,
    /*STATUS_WARN    */ 0xF81F,   // magenta — vector-arcade "warn"
    /*STATUS_WARN_DIM*/ 0x4008,   // dim magenta
    /*STATUS_INFO    */ 0x07FF,
    /*STATUS_STALE   */ 0x4008,
    /*STATUS_DIM     */ 0x0210,
    /*LABEL          */ 0x4008,
    /*VALUE          */ 0xFFFF,
    /*SAFETY         */ 0x4000,   // deep red — theme-agnostic safety override
  },
  /*fonts=*/{
    /*MICRO */ &Tiny3x3a2pt7b,
    /*BODY  */ &Picopixel,
    /*HEADER*/ &PixelOperator88pt7b,
    /*CLOCK */ &digital_7__mono_14pt7b,
  },
  /*hint_mask=*/ bit(Hint::NEON_OUTLINE),
  /*bracket_open=*/  "<",
  /*bracket_close=*/ ">",
};

// BLADE_RUNNER — late-80s neo-noir HUD (THEME.md §2.4). Saturated
// cyan + orange on deep blue, framed panel border, neon halos.
// Identity comes from the FRAME_BORDER hint plus the cyan/orange
// pair — the typography (shared Pixel Operator header, Org_01 body)
// is deliberately understated so the frame + ink does the work.
constexpr Def kBladeRunner = {
  /*inks=*/{
    /*CHROME         */ 0x07FF,   // cyan (H=180°, V=100%)
    /*CHROME_HALO    */ 0x0000,
    /*HEADER         */ 0x07FF,
    /*HEADER_HALO    */ 0x4140,   // ~25% V orange halo — H=30°, V dropped 4× from 0xFD20 so the halo reads as warm glow shadow under cyan text rather than fighting it for attention. (HSV: halo V ≈ 25% of text V)
    /*HEADER_GLOW    */ 0x4140,
    /*HEADER_DIM     */ 0x0210,
    /*BODY           */ 0x07FF,
    /*BODY_HALO      */ 0x0000,
    /*BODY_GLOW      */ 0x4140,
    /*ACCENT         */ 0xFD20,   // orange pop
    /*ACCENT_MAGENTA */ 0xF81F,
    /*ALERT          */ 0xF800,
    /*GHOST          */ 0x0041,   // ~3% cyan ghost — same dimming pass as Vectrex; lets the bright cyan digits pop off the 18:88
    /*DIVIDER        */ 0x07FF,   // cyan divider matches frame
    /*GIANT_DIGITS   */ 0x07FF,
    /*STATUS_OK      */ 0x07FF,   // cyan "OK"
    /*STATUS_OK_DIM  */ 0x0210,
    /*STATUS_WARN    */ 0xFD20,   // orange "warn"
    /*STATUS_WARN_DIM*/ 0x4080,
    /*STATUS_INFO    */ 0x07FF,
    /*STATUS_STALE   */ 0x4080,
    /*STATUS_DIM     */ 0x0210,
    /*LABEL          */ 0x4080,
    /*VALUE          */ 0x07FF,
    /*SAFETY         */ 0x4000,
  },
  /*fonts=*/{
    /*MICRO */ &Tiny3x3a2pt7b,
    /*BODY  */ &Org_01,                  // 5×6 sans w/ true lowercase — HUD feel
    /*HEADER*/ &PixelOperator88pt7b,
    /*CLOCK */ &digital_7__mono_14pt7b,
  },
  /*hint_mask=*/ bit(Hint::FRAME_BORDER) | bit(Hint::NEON_OUTLINE),
  /*bracket_open=*/  ":",
  /*bracket_close=*/ ":",
};

// LCARS_TOS — Star Trek LCARS-precursor (THEME.md §2.5). Colored
// solid blocks instead of brackets, blocky orange/yellow/red sans
// labels. Identity comes from BLOCK_BARS + FRAME_BORDER + the
// orange-dominant ink palette. Brackets are intentionally empty —
// the BLOCK_BARS primitive (gfx_text.h) draws colored block bars
// in their place; scenes that want LCARS's full look call
// gfx::draw_theme_block_header() instead of prepending bracket
// strings.
constexpr Def kLcars = {
  /*inks=*/{
    /*CHROME         */ 0xFD20,   // orange
    /*CHROME_HALO    */ 0x0000,
    /*HEADER         */ 0xFD20,
    /*HEADER_HALO    */ 0x0000,
    /*HEADER_GLOW    */ 0x0000,   // LCARS doesn't NEON_OUTLINE
    /*HEADER_DIM     */ 0x4080,
    /*BODY           */ 0xFD20,
    /*BODY_HALO      */ 0x0000,
    /*BODY_GLOW      */ 0x0000,
    /*ACCENT         */ 0xFFE0,   // yellow pop per THEME.md §2.5
    /*ACCENT_MAGENTA */ 0xF81F,
    /*ALERT          */ 0xF800,   // red alert
    /*GHOST          */ 0x0820,   // ~5% orange ghost (R=1,G=1) — halved from 0x1840 so the orange digits clearly dominate
    /*DIVIDER        */ 0xFD20,
    /*GIANT_DIGITS   */ 0xFD20,   // orange giant clock
    /*STATUS_OK      */ 0xFFE0,   // yellow "OK"
    /*STATUS_OK_DIM  */ 0x4200,
    /*STATUS_WARN    */ 0xFD20,   // orange "warn"
    /*STATUS_WARN_DIM*/ 0x4080,
    /*STATUS_INFO    */ 0xFFE0,
    /*STATUS_STALE   */ 0x4080,
    /*STATUS_DIM     */ 0x4080,
    /*LABEL          */ 0x4080,
    /*VALUE          */ 0xFD20,
    /*SAFETY         */ 0xF800,   // LCARS red-alert IS safety — same red doubles
  },
  /*fonts=*/{
    /*MICRO */ &Tiny3x3a2pt7b,
    /*BODY  */ &Org_01,                  // 5×6 sans — LCARS sans label feel
    /*HEADER*/ &PixelOperator88pt7b,
    /*CLOCK */ &digital_7__mono_14pt7b,
  },
  /*hint_mask=*/ bit(Hint::FRAME_BORDER) | bit(Hint::BLOCK_BARS),
  /*bracket_open=*/  "",   // BLOCK_BARS replaces bracket glyphs
  /*bracket_close=*/ "",
};

// Order MUST match Id enum.
constexpr const Def* kThemes[static_cast<int>(Id::COUNT)] = {
  &kApollo,         // APOLLO_AMBER
  &kNostromo,       // NOSTROMO_GREEN  (T.5)
  &kVectrex,        // VECTREX_NEON    (T.7)
  &kBladeRunner,    // BLADE_RUNNER    (T.7)
  &kLcars,          // LCARS_TOS       (T.7)
};

// ── Per-theme background duotone ramps (T.8 / THEME.md §6) ─────────
//
// Non-default themes opt in to BG retoning by declaring a 4-stop
// ramp: BG_BLACK → BG_SHADOW → BG_HIGHLIGHT → BG_WHITE. At theme-switch
// time we precompute a 256-entry RGB565 LUT by linear interpolation
// between the stops, then for each themable image map its 192-entry
// luminance table through the LUT into a per-image runtime palette
// double-buffer. APOLLO_AMBER carries `present=false` so its
// active_image_palette() lookup short-circuits to the baked palette
// (THEME.md §6.3 — default is bit-for-bit passthrough).
//
// Stops are 0xRRGGBB (24-bit) so the table reads exactly as it does in
// THEME.md §2 — converted to RGB565 once, at boot.
struct BgRamp {
  bool     present;
  uint32_t stop[4];   // BG_BLACK, BG_SHADOW, BG_HIGHLIGHT, BG_WHITE
};

// Order MUST match Id enum.
constexpr BgRamp kBgRamps[static_cast<int>(Id::COUNT)] = {
  /*APOLLO_AMBER  */ { false, {0, 0, 0, 0} },                          // passthrough
  /*NOSTROMO_GREEN*/ { true,  {0x000000, 0x003000, 0x40FF40, 0xD8FFD8} },
  /*VECTREX_NEON  */ { true,  {0x000000, 0xFF00FF, 0x00FFFF, 0xFFFFFF} },
  /*BLADE_RUNNER  */ { true,  {0x000000, 0xFF7000, 0x00E0FF, 0xE0F8FF} },
  /*LCARS_TOS     */ { true,  {0x000000, 0xCC0000, 0xFF9933, 0xFFE080} },
};

constexpr uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
  // Round-to-nearest (matches the bmp_to_header.py packing).
  const uint8_t r5 = static_cast<uint8_t>((r * 31 + 127) / 255);
  const uint8_t g6 = static_cast<uint8_t>((g * 63 + 127) / 255);
  const uint8_t b5 = static_cast<uint8_t>((b * 31 + 127) / 255);
  return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

// Extract 8-bit channels from 0xRRGGBB.
constexpr uint8_t hi_r(uint32_t c) { return static_cast<uint8_t>((c >> 16) & 0xFF); }
constexpr uint8_t hi_g(uint32_t c) { return static_cast<uint8_t>((c >>  8) & 0xFF); }
constexpr uint8_t hi_b(uint32_t c) { return static_cast<uint8_t>( c        & 0xFF); }

// Build the 256-entry ramp LUT from a BgRamp's stops. The four stops
// land at lum {0, 85, 170, 255}; segments interpolate linearly in
// 8-bit RGB then quantise to RGB565 (gamma already baked into the
// stops by the artist's eye — this stays in the same space the image
// was authored in, matching bmp_to_header.py's pre-quantise approach).
void build_ramp_lut(const BgRamp& r, uint16_t out[256]) {
  static constexpr int kStopLum[4] = {0, 85, 170, 255};
  for (int i = 0; i < 4; ++i) {
    const uint32_t c = r.stop[i];
    out[kStopLum[i]] = pack565(hi_r(c), hi_g(c), hi_b(c));
  }
  for (int seg = 0; seg < 3; ++seg) {
    const uint32_t a = r.stop[seg];
    const uint32_t b = r.stop[seg + 1];
    const int      l0 = kStopLum[seg];
    const int      l1 = kStopLum[seg + 1];
    const int      span = l1 - l0;
    const int ar = hi_r(a), ag = hi_g(a), ab = hi_b(a);
    const int br = hi_r(b), bg = hi_g(b), bb = hi_b(b);
    for (int l = l0 + 1; l < l1; ++l) {
      const int t = l - l0;
      const uint8_t cr = static_cast<uint8_t>(ar + (br - ar) * t / span);
      const uint8_t cg = static_cast<uint8_t>(ag + (bg - ag) * t / span);
      const uint8_t cb = static_cast<uint8_t>(ab + (bb - ab) * t / span);
      out[l] = pack565(cr, cg, cb);
    }
  }
}

// Per-image runtime palette double-buffer (THEME.md §6.5). Sized by
// the auto-generated registry. Atomic active-index byte per image —
// same naturally-aligned uint8_t pattern as s_active_id.
//
// kImageRegistryCount is constexpr from bitmaps/_index.h. Guard the
// degenerate empty-registry build (defensive — current asset set has
// 5 entries, but the codegen emits a 1-entry placeholder when no BMPs
// exist).
constexpr int kRtPalSlots =
    (kImageRegistryCount > 0) ? kImageRegistryCount : 1;

uint16_t        s_runtime_palette[kRtPalSlots][2][192] = {};
volatile uint8_t s_active_buffer[kRtPalSlots] = {};

// Rebuild every themable image's runtime palette under the new theme,
// then atomically flip each image's active buffer so Core 1 picks the
// new palette up next frame. Caller is the writer (Core 0); the only
// callers are theme::set() and theme::cycle() so single-writer holds.
//
// Costs (THEME.md §6.6): 256 ramp lerps once + 192 LUT writes per
// image. With 5 images this is ~1.2 K table writes — well under the
// 5 ms budget.
void rebuild_runtime_palettes(uint8_t new_id) {
  if (new_id >= static_cast<uint8_t>(Id::COUNT)) return;
  const BgRamp& r = kBgRamps[new_id];
  if (!r.present) return;   // APOLLO passthrough — nothing to build

  uint16_t lut[256];
  build_ramp_lut(r, lut);

  for (int i = 0; i < kImageRegistryCount; ++i) {
    const ImageEntry& e = kImageRegistry[i];
    if (!e.themeable || e.lum == nullptr) continue;
    const uint8_t inactive = static_cast<uint8_t>(s_active_buffer[i] ^ 1u);
    uint16_t* dst = s_runtime_palette[i][inactive];
    for (int k = 0; k < 192; ++k) {
      dst[k] = lut[e.lum[k]];
    }
    // Publish the freshly-built buffer. Naturally-aligned byte store
    // is atomic on RP2040 — Core 1's next active_image_palette() read
    // will pick it up. No torn frames (FR-15.4).
    s_active_buffer[i] = inactive;
  }
}

// Active id. Naturally-aligned uint8_t — atomic on RP2040, no mutex.
// Same pattern as g_render_fps in main.cpp. Default = APOLLO_AMBER per
// FR-15.2 (boots to default; HA pushes desired theme on connect, T.4).
volatile uint8_t s_active_id = static_cast<uint8_t>(Id::APOLLO_AMBER);

// millis() at the most recent theme change. 0 until the first switch
// so the giant clock's "theme changed" banner doesn't fire on boot.
// 32-bit aligned word store on RP2040 — atomic single-writer (Core 0
// owns set()/cycle()).
volatile uint32_t s_last_change_ms = 0;

// Resolve the active Def. Defensive against an out-of-range s_active_id
// (which set() prevents, but a corrupted memory read shouldn't crash).
inline const Def& active_def() {
  const uint8_t id = s_active_id;
  if (id >= static_cast<uint8_t>(Id::COUNT)) {
    return *kThemes[static_cast<int>(Id::APOLLO_AMBER)];
  }
  return *kThemes[id];
}

}  // namespace

void set(Id id) {
  // Guard against unknown ids (FR-1.3 spirit at the cross-layer
  // boundary — MQTT validates the string, this validates the enum).
  if (static_cast<uint8_t>(id) >= static_cast<uint8_t>(Id::COUNT)) {
    return;
  }
  // T.8 ordering: rebuild runtime palettes BEFORE flipping s_active_id
  // so any reader (Core 1) that observes the new id finds the matching
  // runtime palette already published. APOLLO short-circuits inside
  // rebuild_runtime_palettes() — no work for the default theme.
  const uint8_t new_id = static_cast<uint8_t>(id);
  rebuild_runtime_palettes(new_id);
  s_active_id = new_id;
  s_last_change_ms = ::millis();
}

void cycle(int8_t delta) {
  const uint8_t n   = static_cast<uint8_t>(Id::COUNT);
  const uint8_t cur = s_active_id < n
                          ? s_active_id
                          : static_cast<uint8_t>(Id::APOLLO_AMBER);
  // (cur + n + (delta % n)) % n — handles -1 without underflow on uint8_t.
  const int    step = delta % static_cast<int>(n);
  const uint8_t nxt = static_cast<uint8_t>((cur + n + step) % n);
  // Route through set() so the runtime-palette rebuild + ordered store
  // happens here too (FR-17.10 IR cycle path).
  set(static_cast<Id>(nxt));
}

Id current() {
  return static_cast<Id>(s_active_id);
}

uint16_t ink(Ink role) {
  const uint8_t i = static_cast<uint8_t>(role);
  if (i >= static_cast<uint8_t>(Ink::COUNT)) return 0x0000;
  return active_def().inks[i];
}

const GFXfont* font(FontRole r) {
  const uint8_t i = static_cast<uint8_t>(r);
  if (i >= static_cast<uint8_t>(FontRole::COUNT)) return nullptr;
  return active_def().fonts[i];
}

bool has(Hint h) {
  return (active_def().hint_mask & bit(h)) != 0u;
}

const char* bracket_open()  { return active_def().bracket_open; }
const char* bracket_close() { return active_def().bracket_close; }

uint32_t last_change_ms() { return s_last_change_ms; }

// Wire-id table. Order MUST match Id enum so a single index serves
// both directions of the mapping. Strings are the lowercase enumerator
// names per FR-15.2's example payload `{"id":"apollo_amber"}`.
namespace {
constexpr const char* kWireIds[static_cast<int>(Id::COUNT)] = {
  "apollo_amber",     // APOLLO_AMBER
  "nostromo_green",   // NOSTROMO_GREEN
  "vectrex_neon",     // VECTREX_NEON
  "blade_runner",     // BLADE_RUNNER
  "lcars_tos",        // LCARS_TOS
};
}  // namespace

bool id_from_string(const char* s, Id* out) {
  if (s == nullptr || s[0] == '\0' || out == nullptr) return false;
  for (uint8_t i = 0; i < static_cast<uint8_t>(Id::COUNT); ++i) {
    if (strcmp(s, kWireIds[i]) == 0) {
      *out = static_cast<Id>(i);
      return true;
    }
  }
  return false;
}

const char* string_from_id(Id id) {
  const uint8_t i = static_cast<uint8_t>(id);
  if (i >= static_cast<uint8_t>(Id::COUNT)) return "unknown";
  return kWireIds[i];
}

namespace {
// Display labels — uppercased, words separated by spaces. Indexed by
// Id; order MUST match the enum / kWireIds. Used by the giant clock's
// "theme changed" banner; storing the strings here keeps the wire id
// canonical (lowercase snake_case) while letting the on-screen label
// breathe with whatever capitalization reads best on a 64×32 panel.
constexpr const char* kDisplayNames[static_cast<int>(Id::COUNT)] = {
  "APOLLO AMBER",     // APOLLO_AMBER
  "NOSTROMO GREEN",   // NOSTROMO_GREEN
  "VECTREX NEON",     // VECTREX_NEON
  "BLADE RUNNER",     // BLADE_RUNNER
  "LCARS TOS",        // LCARS_TOS
};
}  // namespace

const char* display_name(Id id) {
  const uint8_t i = static_cast<uint8_t>(id);
  if (i >= static_cast<uint8_t>(Id::COUNT)) return "UNKNOWN";
  return kDisplayNames[i];
}

palette::Id bg_palette_for(BgType bg) {
  // FR-15.6: APOLLO_AMBER is passthrough — return the palette id each
  // background renderer is currently hardcoded to use, so wiring this
  // call into the renderers in a later step produces zero visual delta.
  // Non-default themes will override in T.8 by synthesizing a duotone
  // ramp on theme switch.
  switch (bg) {
    case BgType::STARFIELD: return palette::Id::NIGHT_SKY;
    case BgType::PARALLAX:  return palette::Id::NIGHT_SKY;
    case BgType::NEBULA:    return palette::Id::NEBULA_CLOUDS;
    case BgType::BITMAP:    return palette::Id::NEBULA_CLOUDS;
    case BgType::IMAGE:     return palette::Id::NEBULA_CLOUDS;
    case BgType::NONE:      return palette::Id::NEBULA_CLOUDS;
  }
  return palette::Id::NEBULA_CLOUDS;
}

const uint16_t* active_image_palette(const ImageEntry& e) {
  // Non-themable images always render in their baked palette
  // (THEME.md §6.4 .notheme opt-out).
  if (!e.themeable || e.lum == nullptr) {
    return e.palette;
  }
  const uint8_t id = s_active_id;
  if (id >= static_cast<uint8_t>(Id::COUNT) || !kBgRamps[id].present) {
    // APOLLO + any future passthrough theme: baked palette unchanged.
    return e.palette;
  }
  // Defensive: if the registry index is out of range, fall back to the
  // baked palette rather than indexing past the runtime buffer.
  if (e.image_index >= kRtPalSlots) {
    return e.palette;
  }
  const uint8_t buf = s_active_buffer[e.image_index] & 1u;
  return s_runtime_palette[e.image_index][buf];
}

}  // namespace theme

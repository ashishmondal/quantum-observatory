// See include/theme.h. T.2 skeleton: APOLLO_AMBER only, values mirror
// today's hardcoded literals across scenes/* and gfx_text.h so the
// T.3 refactor produces zero visual delta.

#include "theme.h"

#include <string.h>

#include <Adafruit_GFX.h>      // GFXfont
#include <Fonts/Picopixel.h>
#include <Fonts/Tiny3x3a2pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

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
    /*GHOST          */ 0x0841,   // dim grey, matches giant_clock LCD ghost
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
    /*HEADER*/ &FreeSansBold9pt7b,       // placeholder; T.6 swaps to Press Start 2P
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
    /*GHOST          */ 0x0140,   // dim green LCD ghost
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
    // T.5 reuses Apollo's font roster; T.6 swaps BODY → TomThumb +
    // HEADER → VT323 once the new TTFs are converted.
    /*MICRO */ &Tiny3x3a2pt7b,
    /*BODY  */ &Picopixel,
    /*HEADER*/ &FreeSansBold9pt7b,
    /*CLOCK */ &digital_7__mono_14pt7b,
  },
  /*hint_mask=*/ bit(Hint::SCANLINES) | bit(Hint::CURSOR_BLOCK),
  /*bracket_open=*/  ">",
  /*bracket_close=*/ "_",
};

// Empty placeholder used until T.7 lands the real Defs for VECTREX /
// BLADE_RUNNER / LCARS. Until then, those slots fall back to
// neutral-ish defaults so a non-implemented id leaking through still
// renders a readable panel.
constexpr Def kPlaceholder = {
  /*inks=*/{
    // Order matches Ink enum (T.3b expansion). Neutral-ish defaults
    // keep the panel readable if a non-Apollo id leaks through before
    // T.5 / T.7 land their real Defs.
    0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000,  // chrome, halo, header, halo, glow, header_dim
    0xFFFF, 0x0000, 0x0000,                          // body, halo, glow
    0xFFFF, 0xF81F, 0xF800,                          // accent, accent_magenta, alert
    0x0841, 0x0300, 0xFFFF,                          // ghost, divider, giant_digits
    0x07E0, 0x0140, 0xFD20, 0x6A00,                  // status_ok / status_ok_dim / warn / warn_dim
    0x07FF, 0xC100, 0x630C,                          // info, stale, dim
    0x31A6, 0xCE79, 0x4000,                          // label, value, safety
  },
  /*fonts=*/{ &Tiny3x3a2pt7b, &Picopixel, &FreeSansBold9pt7b, &digital_7__mono_14pt7b },
  /*hint_mask=*/ 0u,
  /*bracket_open=*/  "[",
  /*bracket_close=*/ "]",
};

// Order MUST match Id enum.
constexpr const Def* kThemes[static_cast<int>(Id::COUNT)] = {
  &kApollo,         // APOLLO_AMBER
  &kNostromo,       // NOSTROMO_GREEN  (T.5)
  &kPlaceholder,    // VECTREX_NEON    (T.7)
  &kPlaceholder,    // BLADE_RUNNER    (T.7)
  &kPlaceholder,    // LCARS_TOS       (T.7)
};

// Active id. Naturally-aligned uint8_t — atomic on RP2040, no mutex.
// Same pattern as g_render_fps in main.cpp. Default = APOLLO_AMBER per
// FR-15.2 (boots to default; HA pushes desired theme on connect, T.4).
volatile uint8_t s_active_id = static_cast<uint8_t>(Id::APOLLO_AMBER);

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
  s_active_id = static_cast<uint8_t>(id);
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
    case BgType::SKY:       return palette::Id::NEBULA_CLOUDS;
    case BgType::NONE:      return palette::Id::NEBULA_CLOUDS;
  }
  return palette::Id::NEBULA_CLOUDS;
}

}  // namespace theme

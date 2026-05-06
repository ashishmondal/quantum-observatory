// See include/theme.h. T.2 skeleton: APOLLO_AMBER only, values mirror
// today's hardcoded literals across scenes/* and gfx_text.h so the
// T.3 refactor produces zero visual delta.

#include "theme.h"

#include <Adafruit_GFX.h>      // GFXfont
#include <Fonts/Picopixel.h>
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
//   - accent / alert: not currently referenced as literals; values
//     chosen to match the docs/THEME.md §2.1 spec so T.3 + T.5+ scenes
//     have something to consume from day one.
constexpr Def kApollo = {
  /*inks=*/{
    /*CHROME      */ 0xFFFF,
    /*CHROME_HALO */ 0x0000,
    /*HEADER      */ 0xFFFF,
    /*HEADER_HALO */ 0x0000,
    /*HEADER_GLOW */ 0x0000,   // Apollo doesn't NEON_OUTLINE; unused
    /*BODY        */ 0xF940,   // deep amber, matches giant_clock date
    /*BODY_HALO   */ 0x0000,
    /*BODY_GLOW   */ 0x0000,   // unused under Apollo
    /*ACCENT      */ 0xFFFF,   // white pop (THEME.md §2.1)
    /*ALERT       */ 0xF800,   // red — reserved for priority callouts
    /*GHOST       */ 0x0841,   // dim grey, matches giant_clock LCD ghost
    /*DIVIDER     */ 0x0300,   // dim warm green, matches giant_clock hline
    /*GIANT_DIGITS*/ 0xFFFF,   // white, matches giant_clock live digits
  },
  /*fonts=*/{
    /*CHROME      */ &Picopixel,             // matches draw_clock_chrome
    /*HEADER      */ &FreeSansBold9pt7b,     // matches draw_header (placeholder, T.6 swaps to Press Start 2P)
    /*BODY        */ &Picopixel,             // matches draw_body + most scenes
    /*GIANT_DIGITS*/ &digital_7__mono_14pt7b,
  },
  /*hint_mask=*/ bit(Hint::GIANT_DIGIT_GHOST),
  /*bracket_open=*/  "[",
  /*bracket_close=*/ "]",
};

// Empty placeholder used until T.5 / T.7 land their real Defs. Until
// then, set() will reject any non-Apollo id (clamped to APOLLO_AMBER)
// so the live theme is always populated.
constexpr Def kPlaceholder = {
  /*inks=*/{
    0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000,
    0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xF800,
    0x0841, 0x0300, 0xFFFF,
  },
  /*fonts=*/{ &Picopixel, &FreeSansBold9pt7b, &Picopixel, &digital_7__mono_14pt7b },
  /*hint_mask=*/ 0u,
  /*bracket_open=*/  "[",
  /*bracket_close=*/ "]",
};

// Order MUST match Id enum.
constexpr const Def* kThemes[static_cast<int>(Id::COUNT)] = {
  &kApollo,         // APOLLO_AMBER
  &kPlaceholder,    // NOSTROMO_GREEN  (T.5)
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

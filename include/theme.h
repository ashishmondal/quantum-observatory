// Retro sci-fi theming system (FR-15).
//
// A theme bundles inks, fonts, brackets, layout hints, and a background
// palette policy. Scenes read theme state via this API every frame —
// they MUST NOT hardcode ink colors, font selections, or bracket strings
// (FR-15.3). See docs/THEME.md for the full design.
//
// Concurrency:
//   set()      — Core 0 writer (MQTT handler, in T.4).
//   current()  — readable from either core.
//   ink/font/has/bracket_*/bg_palette_for — readers, called from Core 1
//   render path every frame.
//
// The active id is a single naturally-aligned uint8_t — atomic on RP2040,
// no mutex needed (same rationale as g_render_fps in main.cpp). Theme
// switches take effect at the next frame boundary, no torn frames
// (FR-15.4).
//
// T.2 skeleton: APOLLO_AMBER only, values chosen to reproduce today's
// look bit-for-bit so T.3 (the per-scene refactor) is a pure
// search-and-replace with zero visual delta.

#pragma once

#include <stdint.h>

#include "color_palette.h"

// GFXfont is `typedef struct { ... } GFXfont;` with an anonymous tag in
// Adafruit_GFX, so it cannot be forward-declared. Pull the small header
// directly — it has no other dependencies.
#include <gfxfont.h>

enum class BgType : uint8_t;  // backgrounds.h
struct ImageEntry;            // bitmaps/_index.h (auto-generated)

namespace theme {

// Ordered append-only — values may be logged or persisted in future.
enum class Id : uint8_t {
  APOLLO_AMBER   = 0,   // 70s NASA MOCR — default
  NOSTROMO_GREEN = 1,   // 80s Alien CRT (T.5)
  VECTREX_NEON   = 2,   // Atari/Vectrex vector (T.7)
  BLADE_RUNNER   = 3,   // Neo-noir cyan/orange (T.7)
  LCARS_TOS      = 4,   // Star Trek LCARS-precursor (T.7)
  COUNT
};

// Color roles every scene resolves through theme::ink(). The names are
// deliberately role-based, not hue-based, so a theme can repaint without
// every scene knowing — see FR-15.3.
//
// The STATUS_* family carries semantic state (overhead, daylight, no
// fresh data, below horizon) that the typewriter scenes (iss_pass,
// jupiter_visibility, moon_phase, constellation_now) all share — they
// were duplicating these as constexpr literals before T.3b. Each
// scene-identity HEADER also gets a *_DIM sibling so the per-scene
// pulsing header lands on a theme-owned dim value rather than a
// scene-local literal.
enum class Ink : uint8_t {
  CHROME,           // small HH:MM clock chrome ink
  CHROME_HALO,      // halo behind chrome
  HEADER,           // bracketed scene header (e.g. "[ISS]")
  HEADER_HALO,
  HEADER_GLOW,      // 2nd halo color for NEON_OUTLINE themes
  HEADER_DIM,       // pulsing header low value (generic)
  BODY,             // typewriter / data lines
  BODY_HALO,
  BODY_GLOW,
  ACCENT,           // value-of-interest pop (white)
  ACCENT_MAGENTA,   // second accent (e.g. ISS crew, constellation highlight)
  ALERT,            // priority callouts (constellation highlight star)
  GHOST,            // unlit-segment ghost behind LCD digits
  DIVIDER,          // 1-px separator rows
  GIANT_DIGITS,     // big HH:MM ink
  STATUS_OK,        // green — overhead + visible / healthy
  STATUS_OK_DIM,    // dim green — pulse-low for OK-identity headers (ISS)
  STATUS_WARN,      // amber — countdown, daylight wash, badges (offline, JUP day)
  STATUS_WARN_DIM,  // dim amber — pulse-low for WARN-identity headers (JUP)
  STATUS_INFO,      // cyan — informational data (altitude, magnitude, IAU)
  STATUS_STALE,     // dim amber-red — no fresh data ("WAIT")
  STATUS_DIM,       // dim grey — below horizon
  LABEL,            // ~20% white — dim label (moon "ILL "/"AGE ")
  VALUE,            // ~80% white — bright value next to dim label
  SAFETY,           // deep red — night / thermal_safe (low LED current)
  COUNT
};

enum class FontRole : uint8_t {
  // Ascending size ladder (FR-15.9). Order in this enum reflects
  // physical size; per-theme tables in theme.cpp follow the same order.
  // MICRO and CLOCK are fixed across every theme; BODY and HEADER are
  // the only per-theme slots.
  MICRO,          // 1–3 char indicators only — Tiny3x3 every theme
  BODY,           // readable lines (typewriter / data)
  HEADER,         // bracketed scene header band (e.g. "[ISS]")
  CLOCK,          // giant HH:MM — Digital-7 14pt every theme
  COUNT
};

// Layout hints scenes / overlays opt into. `theme::has(Hint)` returns
// true iff the active theme declares it.
enum class Hint : uint8_t {
  GIANT_DIGIT_GHOST,  // draw "18:88" behind live HH:MM (Apollo)
  SCANLINES,          // every other row dimmed (Nostromo)
  CURSOR_BLOCK,       // blinking block after each header (Nostromo)
  NEON_OUTLINE,       // halo in glow color, not black (Vectrex/BR)
  FRAME_BORDER,       // 1-px outer frame (BR/LCARS)
  BLOCK_BARS,         // colored block instead of bracket glyph (LCARS)
  COUNT
};

// Writer (Core 0). No-op when id is unchanged. Unknown ids (>= COUNT)
// are silently ignored — callers validate at the MQTT boundary.
void set(Id id);

// Cycle the active theme by `delta` steps (FR-17.10 / IR.5). +1 = next,
// -1 = previous; wraps modulo `Id::COUNT`. Same atomic single-byte
// store as set(); safe to call from Core 0 only.
void cycle(int8_t delta);

// Reader. Cheap — single byte load.
Id current();

// Wire-id <-> Id mapping (FR-15.2). Wire ids are lowercase snake_case
// matching the Id enumerators (`apollo_amber`, `nostromo_green`, ...).
// id_from_string returns false on unknown / nullptr / empty input.
// string_from_id returns "unknown" for an out-of-range id (defensive
// — set() prevents it, but the heartbeat path should never crash on a
// corrupt read).
bool        id_from_string(const char* s, Id* out);
const char* string_from_id(Id id);

// Human-friendly display label for `id` (uppercased, words separated
// by spaces — e.g. "APOLLO AMBER"). Stable pointer per id; safe to
// stash. Returns "UNKNOWN" for an out-of-range id.
const char* display_name(Id id);

// Wall-clock (millis()) timestamp of the most recent theme change.
// Set by both set() and cycle(). 0 until the first switch — boot
// remains on APOLLO_AMBER without triggering the on-screen banner.
// Reader-side; safe from Core 1 every frame.
uint32_t last_change_ms();

// Active-theme accessors. All read `current()` internally so callers
// don't have to plumb Id through their call stacks.
uint16_t        ink(Ink role);
const GFXfont*  font(FontRole r);   // nullptr = use built-in 5x7
bool            has(Hint h);

// Bracket strings for headers. Returns "" for themes that use
// BLOCK_BARS instead of glyph brackets.
const char*     bracket_open();
const char*     bracket_close();

// FR-15.6: maps a logical background type to the palette::Id the active
// theme wants used. Default theme (APOLLO_AMBER) returns the same ids
// the existing renderers already use — passthrough, no visual change
// until non-default themes ship.
palette::Id     bg_palette_for(BgType bg);

// FR-15.6 / THEME.md §6: returns the palette `ImagePaletteBg` should
// render `e` with under the active theme. APOLLO_AMBER and any image
// with `themeable=false` always return the baked `e.palette`. Other
// themes return the per-image runtime palette synthesized at the most
// recent theme switch (double-buffered, atomic flip — FR-15.4 next-frame
// swap, no torn frames). Reader-side; safe from Core 1 every frame.
const uint16_t* active_image_palette(const ImageEntry& e);

}  // namespace theme

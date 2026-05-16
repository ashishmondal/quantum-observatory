// Retro sci-fi theming system (FR-15).
//
// A theme bundles inks, fonts, brackets, layout hints, a background
// palette policy, and an optional animated clock-screen background.
// Scenes read theme state via this API every frame — they MUST NOT
// hardcode ink colors, font selections, or bracket strings (FR-15.3).
// See docs/THEME.md for the full design.
//
// OO model (T.x refactor): each theme is its own class derived from
// `theme::Theme`. The base class stores the static metadata (inks,
// fonts, hint mask, brackets, optional duotone bg ramp) via const
// pointers passed in the ctor; concrete subclasses live one-per-file
// under include/themes/ + src/themes/ and override the per-theme
// virtuals (`bg_palette_for`, `init_clock_bg`, `render_clock_bg`).
// The free-function API (`theme::ink()`, etc.) is preserved — every
// call delegates to `current_theme()` so existing scene code is
// unchanged.
//
// Concurrency:
//   set()           — Core 0 writer (MQTT / IR handler).
//   current() /
//   current_theme() — readable from either core.
//   ink/font/has/bracket_*/bg_palette_for — readers, called from
//                     Core 1 render path every frame.
//
// The active id is a single naturally-aligned uint8_t — atomic on RP2040,
// no mutex needed (same rationale as g_render_fps in main.cpp). Theme
// switches take effect at the next frame boundary, no torn frames
// (FR-15.4). render_clock_bg() may carry mutable per-theme animation
// state; it is single-reader (Core 1) so no extra synchronisation is
// required.

#pragma once

#include <stdint.h>

#include "color_palette.h"

// GFXfont is `typedef struct { ... } GFXfont;` with an anonymous tag in
// Adafruit_GFX, so it cannot be forward-declared. Pull the small header
// directly — it has no other dependencies.
#include <gfxfont.h>

enum class BgType : uint8_t;  // backgrounds.h
struct ImageEntry;            // bitmaps/_index.h (auto-generated)
class Adafruit_Protomatter;   // render_clock_bg() target — full include is heavy

// Forward-declared so theme.h doesn't drag buzzer.h into every TU
// that already pulls theme.h. Concrete definition lives in
// include/buzzer.h; theme.cpp + each theme .cpp include it where
// they actually use the type.
namespace buzzer { struct Note; }

namespace theme {

// Forward decls so the public free functions in this header can
// reference theme::Theme below.
class Theme;

// One per-theme signature melody (FR-10.7). Returned by-value so
// callers don't have to worry about lifetime — the underlying
// `notes` array is always file-scope `static constexpr` in the
// owning theme's .cpp, so it outlives the program.
struct Melody {
  const buzzer::Note* notes;   // nullptr ⇒ theme has no melody
  uint8_t             count;
};

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

// FR-15.6 / THEME.md §6: maps a logical background type to the palette::Id the active
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

// FR-15.6 / THEME.md §6.9 — image-tint strength (0..100, default 50).
// 0   → every themable image renders in its original baked palette
//       under any theme (full passthrough).
// 100 → full duotone retoning (the pre-T.10 behaviour).
// Anything in between is a per-channel linear blend in
// rebuild_runtime_palettes(). Setter clamps, stores, and rebuilds
// every themable image's runtime palette — same double-buffered
// atomic flip a theme switch uses, so FR-15.4 (no torn frames) holds.
// Apollo (passthrough, no bg_ramp) is a no-op at every value.
// Writer is Core 0 only; reader is byte-atomic on RP2040.
void    set_image_tint_pct(uint8_t pct);
uint8_t image_tint_pct();

// FR-10.7 signature-melody accessor. Returns the per-theme audible
// signature played non-blockingly on the rising edge of `set()`. Out-
// of-range ids and themes that declare no melody return `{nullptr, 0}`.
// The trigger lives inside `set()` itself so every theme-switch path
// (MQTT, IR remote, future button) shares one chokepoint.
Melody signature_melody(Id id);

// ── Theme class hierarchy ──────────────────────────────────────────
//
// One concrete subclass per Id (see include/themes/*.h). All inks /
// fonts / hints / brackets live in the base class as const data
// pointers (set once via the ctor); the only per-theme overridable
// behaviour is the optional duotone bg ramp + the animated clock-screen
// background. Subclasses live as global non-const singletons so the
// per-frame render_clock_bg() can carry mutable animation state without
// extra heap allocation.

// 4-stop background duotone ramp (THEME.md §6). Stops are 0xRRGGBB
// (24-bit) — converted to RGB565 once, at theme-switch time. Apollo
// passes nullptr for the ramp pointer to keep its image backgrounds
// in passthrough mode (FR-15.6, default theme is bit-for-bit unchanged).
struct BgRamp {
  uint32_t stop[4];   // BG_BLACK, BG_SHADOW, BG_HIGHLIGHT, BG_WHITE
};

class Theme {
 public:
  // Subclasses pass pointers to their own static const data. The
  // arrays MUST be sized to Ink::COUNT / FontRole::COUNT respectively;
  // the base class indexes into them without re-checking the arity.
  // `bg_ramp` may be nullptr — that signals "passthrough", same
  // behaviour as APOLLO_AMBER's image backgrounds.
  Theme(Id id,
        const char* wire_id,
        const char* display_name,
        const uint16_t* inks,
        const GFXfont* const* fonts,
        uint32_t hint_mask,
        const char* bracket_open,
        const char* bracket_close,
        const BgRamp* bg_ramp);

  virtual ~Theme() = default;

  // Static metadata accessors — non-virtual, one byte / one pointer
  // load each. Hot-path (Core 1, every frame).
  Id              id()            const { return m_id; }
  const char*     wire_id()       const { return m_wire_id; }
  const char*     display_name()  const { return m_display_name; }
  uint16_t        ink(Ink role)   const { return m_inks[static_cast<int>(role)]; }
  const GFXfont*  font(FontRole r) const { return m_fonts[static_cast<int>(r)]; }
  bool            has(Hint h)     const { return (m_hint_mask & (1u << static_cast<uint32_t>(h))) != 0u; }
  const char*     bracket_open()  const { return m_bracket_open; }
  const char*     bracket_close() const { return m_bracket_close; }
  const BgRamp*   bg_ramp()       const { return m_bg_ramp; }

  // Per-theme virtuals.
  //
  //   bg_palette_for: the palette `BackgroundXxxBg` should render
  //   under this theme. Default impl reproduces APOLLO's table; any
  //   subclass with non-passthrough image plans overrides.
  //
  //   init_clock_bg / render_clock_bg: optional per-theme animated
  //   background drawn behind the giant HH:MM. Default impls clear
  //   to black and do nothing — themes that want an animation override
  //   render_clock_bg() and (if they need RNG/state seeding) init_clock_bg().
  //   render_clock_bg() is responsible for fillScreen(); it MUST NOT
  //   call matrix.show() — same contract as the other *Bg classes.
  virtual palette::Id bg_palette_for(BgType bg) const;
  virtual void        init_clock_bg() {}
  virtual void        render_clock_bg(Adafruit_Protomatter& matrix, uint32_t now_ms);

  // FR-10.7 per-theme signature melody. Default = no melody (silent
  // theme swap). Concrete themes override to return a static-storage
  // `Note[]` defined in their .cpp. The melody is played by the
  // free-function `set()` chokepoint, never by subclasses themselves.
  virtual Melody melody() const { return {nullptr, 0}; }

 private:
  Id                    m_id;
  const char*           m_wire_id;
  const char*           m_display_name;
  const uint16_t*       m_inks;
  const GFXfont* const* m_fonts;
  uint32_t              m_hint_mask;
  const char*           m_bracket_open;
  const char*           m_bracket_close;
  const BgRamp*         m_bg_ramp;
};

// Active-theme accessor used by the dispatcher (e.g. backgrounds.cpp's
// THEME_CLOCK case calls `current_theme().render_clock_bg(...)`).
// Same atomic single-byte read as current(); safe from Core 1 every
// frame. Always returns a valid reference — out-of-range ids fall
// back to APOLLO_AMBER.
Theme& current_theme();

}  // namespace theme

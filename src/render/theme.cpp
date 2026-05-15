// See include/theme.h. Per-theme metadata (inks, fonts, hint masks,
// brackets, duotone bg ramp, animated clock-screen background) lives
// in one Theme subclass per id under src/themes/. This file holds:
//
//   - the global theme registry (kThemes[])
//   - the Theme base-class ctor + default virtuals
//   - the free-function public API (theme::ink, font, has, ...)
//   - the active-id atomic byte and ordered theme-switch path
//   - the per-image runtime palette double-buffer (THEME.md §6.5)
//
// Visual delta vs. the pre-OO refactor is zero — every theme's data
// table moved to its dedicated .cpp byte-for-byte.

#include "theme.h"

#include <string.h>

#include <Arduino.h>           // millis()
#include <Adafruit_Protomatter.h>

#include "backgrounds.h"        // BgType
#include "bitmaps/_index.h"
#include "buzzer.h"             // FR-10.7 — play() on rising-edge theme change
#include "color_palette.h"
#include "themes/apollo_amber_theme.h"
#include "themes/blade_runner_theme.h"
#include "themes/lcars_tos_theme.h"
#include "themes/nostromo_green_theme.h"
#include "themes/vectrex_neon_theme.h"

namespace theme {

// ── Theme base class ─────────────────────────────────────────────

Theme::Theme(Id id,
             const char* wire_id,
             const char* display_name,
             const uint16_t* inks,
             const GFXfont* const* fonts,
             uint32_t hint_mask,
             const char* bracket_open,
             const char* bracket_close,
             const BgRamp* bg_ramp)
    : m_id(id),
      m_wire_id(wire_id),
      m_display_name(display_name),
      m_inks(inks),
      m_fonts(fonts),
      m_hint_mask(hint_mask),
      m_bracket_open(bracket_open),
      m_bracket_close(bracket_close),
      m_bg_ramp(bg_ramp) {}

// FR-15.6: APOLLO_AMBER passthrough — return the palette id each
// background renderer is currently hardcoded to use, so the as-shipped
// look is preserved bit-for-bit. Subclasses with non-passthrough image
// plans override.
palette::Id Theme::bg_palette_for(BgType bg) const {
  switch (bg) {
    case BgType::STARFIELD: return palette::Id::NIGHT_SKY;
    case BgType::PARALLAX:  return palette::Id::NIGHT_SKY;
    case BgType::NEBULA:    return palette::Id::NEBULA_CLOUDS;
    case BgType::BITMAP:    return palette::Id::NEBULA_CLOUDS;
    case BgType::IMAGE:     return palette::Id::NEBULA_CLOUDS;
    case BgType::THEME_CLOCK: return palette::Id::NEBULA_CLOUDS;
    case BgType::NONE:      return palette::Id::NEBULA_CLOUDS;
  }
  return palette::Id::NEBULA_CLOUDS;
}

// Default impl: theme has no animated background. Subclasses override.
void Theme::render_clock_bg(Adafruit_Protomatter& matrix, uint32_t /*now_ms*/) {
  matrix.fillScreen(0x0000);
}

// ── Registry ─────────────────────────────────────────────────────

namespace {

// Order MUST match Id enum.
Theme* const kThemes[static_cast<int>(Id::COUNT)] = {
  &g_apollo_amber_theme,    // APOLLO_AMBER
  &g_nostromo_green_theme,  // NOSTROMO_GREEN
  &g_vectrex_neon_theme,    // VECTREX_NEON
  &g_blade_runner_theme,    // BLADE_RUNNER
  &g_lcars_tos_theme,       // LCARS_TOS
};

// Active id. Naturally-aligned uint8_t — atomic on RP2040, no mutex.
// Same pattern as g_render_fps in main.cpp. Default = APOLLO_AMBER per
// FR-15.2 (boots to default; HA pushes desired theme on connect).
volatile uint8_t s_active_id = static_cast<uint8_t>(Id::APOLLO_AMBER);

// millis() at the most recent theme change. 0 until the first switch
// so the giant clock's "theme changed" banner doesn't fire on boot.
volatile uint32_t s_last_change_ms = 0;

inline Theme& active_theme_ref() {
  const uint8_t id = s_active_id;
  if (id >= static_cast<uint8_t>(Id::COUNT)) {
    return *kThemes[static_cast<int>(Id::APOLLO_AMBER)];
  }
  return *kThemes[id];
}

// ── Per-image runtime palette double-buffer (THEME.md §6.5) ──────
//
// Sized by the auto-generated registry. Atomic active-index byte per
// image — same naturally-aligned uint8_t pattern as s_active_id.

constexpr int kRtPalSlots =
    (kImageRegistryCount > 0) ? kImageRegistryCount : 1;

uint16_t        s_runtime_palette[kRtPalSlots][2][192] = {};
volatile uint8_t s_active_buffer[kRtPalSlots] = {};

constexpr uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
  // Round-to-nearest (matches the bmp_to_header.py packing).
  const uint8_t r5 = static_cast<uint8_t>((r * 31 + 127) / 255);
  const uint8_t g6 = static_cast<uint8_t>((g * 63 + 127) / 255);
  const uint8_t b5 = static_cast<uint8_t>((b * 31 + 127) / 255);
  return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

constexpr uint8_t hi_r(uint32_t c) { return static_cast<uint8_t>((c >> 16) & 0xFF); }
constexpr uint8_t hi_g(uint32_t c) { return static_cast<uint8_t>((c >>  8) & 0xFF); }
constexpr uint8_t hi_b(uint32_t c) { return static_cast<uint8_t>( c        & 0xFF); }

// Build the 256-entry ramp LUT from a BgRamp's stops. The four stops
// land at lum {0, 85, 170, 255}; segments interpolate linearly in
// 8-bit RGB then quantise to RGB565.
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

// Rebuild every themable image's runtime palette under the new theme,
// then atomically flip each image's active buffer so Core 1 picks the
// new palette up next frame. Caller is the writer (Core 0); the only
// callers are theme::set() / theme::cycle() so single-writer holds.
void rebuild_runtime_palettes(uint8_t new_id) {
  if (new_id >= static_cast<uint8_t>(Id::COUNT)) return;
  const BgRamp* r = kThemes[new_id]->bg_ramp();
  if (r == nullptr) return;   // passthrough — nothing to build

  uint16_t lut[256];
  build_ramp_lut(*r, lut);

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

}  // namespace

// ── Public API ───────────────────────────────────────────────────

void set(Id id) {
  if (static_cast<uint8_t>(id) >= static_cast<uint8_t>(Id::COUNT)) {
    return;
  }
  // Idempotent set-to-same is silent (FR-10.7) — no melody, no
  // re-init, no s_last_change_ms bump. Catches the common case
  // where HA re-publishes the active theme on reconnect.
  const uint8_t new_id = static_cast<uint8_t>(id);
  if (new_id == s_active_id) return;
  // Ordering: rebuild runtime palettes BEFORE flipping s_active_id
  // so any reader (Core 1) that observes the new id finds the matching
  // runtime palette already published. Passthrough themes short-circuit
  // inside rebuild_runtime_palettes().
  rebuild_runtime_palettes(new_id);
  // Seed the incoming theme's animated clock-background state so the
  // first frame after the switch renders against initialised arrays
  // (Nostromo column tables, BR rain spawns, LCARS cells). Cheap —
  // tens of writes per theme — and runs Core 0 before the id flip
  // so Core 1 never sees a half-built animator.
  kThemes[new_id]->init_clock_bg();
  s_active_id = new_id;
  s_last_change_ms = ::millis();
  // FR-10.7 audible counterpart of the visual theme swap. Fired
  // here (not in subclasses) so every theme-switch path — MQTT,
  // IR remote, future button — shares one chokepoint and a
  // theme-cycle stampede can't stack overlapping melodies
  // (buzzer::play cancels any in-flight sequence).
  const Melody m = kThemes[new_id]->melody();
  if (m.notes != nullptr && m.count > 0) {
    buzzer::play(m.notes, m.count);
  }
}

void cycle(int8_t delta) {
  const uint8_t n   = static_cast<uint8_t>(Id::COUNT);
  const uint8_t cur = s_active_id < n
                          ? s_active_id
                          : static_cast<uint8_t>(Id::APOLLO_AMBER);
  // (cur + n + (delta % n)) % n — handles -1 without underflow on uint8_t.
  const int    step = delta % static_cast<int>(n);
  const uint8_t nxt = static_cast<uint8_t>((cur + n + step) % n);
  set(static_cast<Id>(nxt));
}

Id current() {
  return static_cast<Id>(s_active_id);
}

Theme& current_theme() {
  return active_theme_ref();
}

uint16_t ink(Ink role) {
  const uint8_t i = static_cast<uint8_t>(role);
  if (i >= static_cast<uint8_t>(Ink::COUNT)) return 0x0000;
  return active_theme_ref().ink(role);
}

const GFXfont* font(FontRole r) {
  const uint8_t i = static_cast<uint8_t>(r);
  if (i >= static_cast<uint8_t>(FontRole::COUNT)) return nullptr;
  return active_theme_ref().font(r);
}

bool has(Hint h) {
  return active_theme_ref().has(h);
}

const char* bracket_open()  { return active_theme_ref().bracket_open(); }
const char* bracket_close() { return active_theme_ref().bracket_close(); }

uint32_t last_change_ms() { return s_last_change_ms; }

bool id_from_string(const char* s, Id* out) {
  if (s == nullptr || s[0] == '\0' || out == nullptr) return false;
  for (uint8_t i = 0; i < static_cast<uint8_t>(Id::COUNT); ++i) {
    if (strcmp(s, kThemes[i]->wire_id()) == 0) {
      *out = static_cast<Id>(i);
      return true;
    }
  }
  return false;
}

const char* string_from_id(Id id) {
  const uint8_t i = static_cast<uint8_t>(id);
  if (i >= static_cast<uint8_t>(Id::COUNT)) return "unknown";
  return kThemes[i]->wire_id();
}

const char* display_name(Id id) {
  const uint8_t i = static_cast<uint8_t>(id);
  if (i >= static_cast<uint8_t>(Id::COUNT)) return "UNKNOWN";
  return kThemes[i]->display_name();
}

palette::Id bg_palette_for(BgType bg) {
  return active_theme_ref().bg_palette_for(bg);
}

const uint16_t* active_image_palette(const ImageEntry& e) {
  // Non-themable images always render in their baked palette
  // (THEME.md §6.4 .notheme opt-out).
  if (!e.themeable || e.lum == nullptr) {
    return e.palette;
  }
  const uint8_t id = s_active_id;
  if (id >= static_cast<uint8_t>(Id::COUNT) ||
      kThemes[id]->bg_ramp() == nullptr) {
    return e.palette;
  }
  // Defensive: if the registry index is out of range, fall back to
  // the baked palette rather than indexing past the runtime buffer.
  if (e.image_index >= kRtPalSlots) {
    return e.palette;
  }
  const uint8_t buf = s_active_buffer[e.image_index] & 1u;
  return s_runtime_palette[e.image_index][buf];
}

Melody signature_melody(Id id) {
  const uint8_t i = static_cast<uint8_t>(id);
  if (i >= static_cast<uint8_t>(Id::COUNT)) return {nullptr, 0};
  return kThemes[i]->melody();
}

}  // namespace theme

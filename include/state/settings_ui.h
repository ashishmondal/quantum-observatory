// FR-19 — operator-facing settings overlay model.
//
// Phase S.1: cross-core nav + commit state for the on-panel settings
// menu reachable via the IR remote `*` (Options) button. The model
// lives on Core 0 alongside IR dispatch — every mutator runs from
// the IR action lane (or the on-board MENU lane in a later phase) —
// and Core 1 reads a coherent snapshot once per frame to drive the
// SettingsOverlayLayer (FR-19.5).
//
// Concurrency:
//   toggle_open() / nav_*() / commit_ok() / back() / tick()  Core 0 only.
//   is_open() / snapshot()  any core; mutex guards the multi-field
//                           snapshot copy. is_open() is a single
//                           naturally-aligned byte read so the IR
//                           dispatch hot path can branch without
//                           taking the mutex.
//
// What the model owns vs. doesn't:
//   - Owns: nav state (mode / category / selected item), envelope
//     timestamps (open/close fade, save toast, inverse-flash, idle
//     auto-close).
//   - Doesn't own the values themselves — every actual setting
//     value lives in `prefs::current()`, and every commit goes
//     through `prefs::set_*()`. This keeps a single source of
//     truth (FR-18.7 dirty flag stays meaningful) and lets MQTT
//     edits and menu edits race-free at the same chokepoint.
//
// Why a layer-not-scene: the underlying scene keeps animating
// behind the panel so the BG-tint preview is *live* (the operator
// sees the actual scene background recolour as they hold ◄/►).
// SafetyOverlayLayer set the precedent in D.3.

#pragma once

#include <stdint.h>

// Arduino Common.h defines `DISPLAY` as 0x1 (a tone()/Serial mode
// constant). That collides with our Cat::DISPLAY enum value below
// and causes the entire enum class to fail to parse. Drop the macro
// — none of the firmware uses tone()'s DISPLAY mode.
#ifdef DISPLAY
#undef DISPLAY
#endif

namespace settings_ui {

// FR-19 — top-level menu structure. Two categories shipped in S.1
// (DISPLAY, SOUND); add to the enum AND the per-category Item
// counts when the third lands.
enum class Mode : uint8_t {
  CLOSED   = 0,  // not visible (or fading out)
  ROOT     = 1,  // category picker — two tiles
  CATEGORY = 2,  // category detail rows (DISPLAY tint / SOUND rows)
};

enum class Cat : uint8_t {
  DISPLAY = 0,   // BG TINT row only (S.1)
  SOUND   = 1,   // theme sound / button sound / time-tick sound rows
  COUNT   = 2,
};

// Per-category item indices. Stored in the snapshot as a uint8_t
// so a torn cross-core read still lands inside a defined enum
// range; the layer treats out-of-range as 0.
enum class DisplayItem : uint8_t {
  TINT = 0,
  COUNT = 1,
};
enum class SoundItem : uint8_t {
  THEME      = 0,
  BUTTON     = 1,
  TICK       = 2,
  COUNT      = 3,
};

// Fade envelope direction surfaced in the snapshot so the layer
// can animate the bayer-dither veil over open/close.
enum class FadeDir : uint8_t { NONE = 0, IN = 1, OUT = 2 };

// Render-side snapshot — a coherent copy of the model under the
// mutex. The layer reads this once per frame and consults nothing
// else. All time fields are wall-clock millis() so the layer can
// derive its own animation phases via `now_ms - *_started_ms`.
struct Snapshot {
  Mode    mode;
  Cat     cat;
  uint8_t sel_index;          // category-local item index (clamp on read)
  // Latest applied/visible values mirrored from prefs::current().
  // Sampled at snapshot time so a concurrent MQTT commit can't tear
  // the layer's per-frame redraw between the row label and pill.
  uint8_t       tint_pct;
  bool          theme_sound;
  bool          button_sound;
  uint8_t       tick_sound_mode;  // raw enum byte
  // Animation envelopes.
  FadeDir  fade_dir;
  uint32_t fade_started_ms;    // millis() at the active fade's start
  uint32_t flash_started_ms;   // millis() at the latest value-change flash
  uint32_t toast_started_ms;   // millis() at the latest "SAVED" toast
  uint32_t opened_ms;          // millis() when the menu went visible
  uint32_t last_input_ms;      // millis() of the most recent nav action
};

// One-shot init — registers the mutex. Idempotent. Call from
// main.cpp::setup() before any IR dispatch is live so the first
// `*` press cannot race the mutex initialisation.
void init();

// Toggle the menu open/closed. Bound to the IR `*` (Options)
// button (FR-19.1). Plays the corresponding open/close jingle
// (gated by prefs::current().button_sound, same discipline as the
// IR-press chirp in ir_remote::poll()).
void toggle_open();

// Hard close (no fade, immediate). Used by the on-board MENU
// button's "always go to info overlay" path so opening info from
// across the room while the settings menu happens to be up does
// not leave both layers competing for the panel.
void force_close();

// Cheap byte read — Core 0 dispatch hot path branches on this to
// route up/down/left/right/ok/back into nav_*() instead of the
// global scene-cycle / theme-cycle actions. Single naturally-
// aligned byte; mutex not required.
bool is_open();

// IR navigation (Core 0). The model decides what each direction
// does based on current Mode + Cat. Each call may also fire a UI
// jingle and (for value-changing nav) commit the new value via
// prefs::set_*(). Calls are no-ops when the menu is closed —
// callers should still pre-check is_open() to avoid masking the
// underlying scene-cycle / theme-cycle behaviour.
void nav_up();
void nav_down();
void nav_left();
void nav_right();
void commit_ok();   // ROOT: enter category. CATEGORY: toggle/no-op.
void back();        // CATEGORY: return to ROOT. ROOT: close menu.

// Drives idle auto-close (30 s after last input) and stamps the
// save-toast lifetime on prefs_dirty falling-edge. Cheap when
// nothing is happening.
void tick(uint32_t now_ms);

// Read a coherent snapshot under the mutex. The layer copies it
// into a stack local once per frame and reads from there.
Snapshot snapshot();

}  // namespace settings_ui

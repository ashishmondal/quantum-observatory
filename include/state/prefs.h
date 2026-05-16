// Persistent user preferences (FR-18). Tiny LittleFS-backed store for
// viewer-ergonomics choices that should survive a power cycle without
// round-tripping HA — currently just the active retro sci-fi theme
// (FR-15), with future room for default scene / brightness / mute.
//
// Phase P.1 — skeleton only:
//   - In-RAM cache (Prefs struct) with sane defaults from config.h.
//   - LittleFS mount (CODING_PRACTICES §3 — Core 0 only; the FS lives
//     on the same QSPI flash arduino-pico's network/heap subsystem
//     uses, so Core 1 must never touch it).
//   - current() reader + dirty-flag plumbing.
//   - NO load() / NO save() yet — those are P.2 / P.3. A missing or
//     unmounted file is treated as "no prefs yet" (FR-18.5).
//
// Concurrency:
//   begin() / set_*()    — Core 0 only.
//   current() / is_*()   — readable from either core; the cached
//                          struct is byte-stable once begin() returns
//                          and setter writes are wrapped in s_mutex.
//
// The mutex guards the {cache, dirty, last_setter_ms} triple so the
// debounced writeback tick (P.3) can read a coherent snapshot.

#pragma once

#include <stdint.h>

#include "theme.h"  // theme::Id

namespace prefs {

// On-disk schema version. Bumped only when the JSON shape changes in
// a way the boot-restore parser (P.2) needs to migrate. Callers on
// the in-RAM side don't read this; it's only relevant to load/save.
//
// v1 -> v2 (T.11): added image_tint_pct (FR-15.6 / FR-18.2). v1 files
// on disk load cleanly via the existing P.2 forward-compat passthrough
// — a missing key falls through to the default in kDefaults.
constexpr uint8_t kSchemaVersion = 2;

// Flat record. Add fields here AND in the JSON serialiser/parser
// (P.2/P.3) when the v1 scope grows. POD so we can byte-copy under
// the mutex without worrying about non-trivial constructors.
struct Prefs {
  uint8_t   schema_v;        // = kSchemaVersion in RAM; tracks the
                             // on-disk value once load() lands in P.2.
  theme::Id theme;           // FR-18.2 v1 — active retro sci-fi theme.
  uint8_t   image_tint_pct;  // FR-18.2 v2 / FR-15.6 — image-tint
                             // strength 0..100 (default 50).
};

// Mount the filesystem and initialise the in-RAM cache to defaults.
// Idempotent — safe to call once from setup(). Logs the mount result.
// MUST be called BEFORE the first prefs::current() reader. P.2 wires
// the actual /prefs.json load into this same entry point.
void begin();

// Setter for the active theme (FR-18.3). Calls theme::set(id)
// internally so the next-frame swap (FR-15.4) happens at the same
// point the bare theme:: API would have produced; ALSO updates the
// in-RAM prefs cache, marks it dirty, and arms the debounced
// writeback timer (FR-18.4). Idempotent — a no-op store does not
// mark dirty.
//
// Wire this from every USER-CHOICE theme path (MQTT
// observatory/theme handler, IR `◄`/`►` dispatch). Diagnostic /
// firmware-internal theme switches (e.g. gfx_test cycling, T.2's
// bring-up `theme::set(APOLLO_AMBER)`) MUST NOT go through here —
// they'd pollute the persisted choice.
void set_theme(theme::Id id);

// Cycle the active theme by `delta` steps (FR-18.3 / FR-17.10
// counterpart of theme::cycle()). +1 = next, -1 = previous; wraps
// modulo Id::COUNT. Routes through set_theme() so the IR remote
// path persists exactly the same way the MQTT path does.
void cycle_theme(int8_t delta);

// FR-15.6 / FR-18.2 (v2) — image-tint strength setter (0..100,
// clamped). Mirrors set_theme(): updates theme:: visible state
// first (theme::set_image_tint_pct triggers the next-frame palette
// rebuild per FR-15.4), then under s_mutex updates the in-RAM cache,
// marks dirty, arms the FR-18.4 writeback. Idempotent on no-op.
// Wire from every USER-CHOICE tint path (MQTT observatory/theme
// `tint` field). Diagnostic / firmware-internal calls SHALL NOT
// route through here.
void set_image_tint_pct(uint8_t pct);

// Wear-protected writeback tick (FR-18.4). Call from the Core 0
// loop() each iteration; cheap when nothing is dirty. Flushes the
// in-RAM cache to /prefs.json iff ALL of:
//   - cache is dirty,
//   - ≥ 5 s since the last set_*() call (settle window),
//   - ≥ 30 s since the last successful flush (rate cap),
//   - LittleFS mounted successfully at boot.
// Atomic write (write-to-temp + rename) so a crash mid-write
// cannot corrupt /prefs.json.
void tick(uint32_t now_ms);

// Snapshot accessor. Returns a const reference to the in-RAM cache.
// Safe to read from either core — the cache is only mutated by
// Core 0 setters (none in P.1) so a render-side reader sees either
// the pre- or post-write state, never a torn intermediate.
const Prefs& current();

// True iff the LittleFS partition mounted successfully. P.2 / P.3
// gate their load + writeback logic on this so an unformatted or
// missing partition degrades gracefully (defaults apply, nothing
// persists) rather than crashing.
bool is_mounted();

// True iff the in-RAM cache differs from the last value successfully
// written to flash (FR-18.7). P.1 always returns false — there are
// no setters yet. The status-heartbeat plumbing (P.5) will read
// this; surfacing it now keeps the API surface stable across phases.
bool is_dirty();

// Factory-reset escape hatch (FR-18.8). Deletes /prefs.json from
// LittleFS and clears the in-RAM dirty flag so the writeback tick
// won't immediately re-create it. The caller is expected to follow
// up with `rp2040.reboot()` — boot-restore (FR-18.5) then sees a
// missing file and applies stock defaults. Safe no-op when the
// filesystem isn't mounted (the reboot still happens; defaults
// were already in effect). Core 0 only — same as every other
// LittleFS entry point in this module.
void reset();

// Forward-version passthrough (FR-18.5). On boot, load() captures
// any unknown top-level keys from /prefs.json into a static buffer
// so P.3's writeback can re-emit them verbatim — a downgrade from a
// future schema must not silently drop forward-version data.
//
// Returns a NUL-terminated JSON fragment of the form
//   ,"unknown_key":<value>,"another":<value>
// (i.e. each unknown entry pre-pended with `,` and ready to splice
// after the last known field) or an empty string when no unknown
// keys were seen. Pointer is valid for the program lifetime.
const char* unknown_passthrough();

}  // namespace prefs

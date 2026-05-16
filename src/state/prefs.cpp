// Persistent user preferences (FR-18) — Phase P.1 skeleton.
// See include/state/prefs.h for the contract. P.2 adds /prefs.json
// boot-restore; P.3 adds the debounced wear-protected writeback.

#include "prefs.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <pico/mutex.h>

namespace prefs {

namespace {

// Defaults applied when no /prefs.json exists (FR-18.5). Theme falls
// back to APOLLO_AMBER per FR-15.2 / plan P.1; matches the explicit
// theme::set() that lived in setup() before this module landed.
constexpr Prefs kDefaults = {
    .schema_v = kSchemaVersion,
    .theme    = theme::Id::APOLLO_AMBER,
};

// In-RAM cache. Read-mostly: only mutated by Core 0 setters (none in
// P.1) under s_mutex. Render-side readers (Core 1) take a const
// reference via current() and copy out anything they need; the POD
// layout means a torn read is impossible without a setter, and
// P.1 has none.
Prefs s_cache = kDefaults;

// Guards {s_cache, s_dirty} for P.3's writeback tick — the debounced
// flush will need a coherent snapshot of "what's the current value
// AND is it different from what's on flash". Initialised in begin().
mutex_t s_mutex;

bool s_mounted = false;  // LittleFS.begin() result — gates P.2/P.3 I/O.
bool s_dirty   = false;  // Always false in P.1; P.3 flips it on setter.

}  // namespace

void begin() {
  // mutex_init() is idempotent on the SDK but we want a single,
  // documented init site to mirror scene_state::init() / tod::init().
  mutex_init(&s_mutex);

  // Mount the on-board QSPI filesystem partition (sized via
  // platformio.ini board_build.filesystem_size). LittleFS.begin()
  // formats the partition automatically on first boot, so a fresh
  // device just logs "mounted" and current() returns kDefaults.
  s_mounted = LittleFS.begin();
  if (s_mounted) {
    Serial.println("[prefs] LittleFS mounted");
  } else {
    // Treat as "no prefs yet" — defaults apply (FR-18.5). Don't
    // retry; the most likely cause is a zero-sized partition (config
    // error) and burning watchdog budget on retries won't help.
    Serial.println("[prefs] LittleFS mount failed — defaults apply, "
                   "writeback disabled");
  }

  // P.2 will load /prefs.json here and apply each known key
  // (theme::set(s_cache.theme) etc.). P.1 leaves the cache at
  // defaults — the explicit theme::set(APOLLO_AMBER) in setup()
  // remains the authoritative bring-up call until P.2 lands.
}

const Prefs& current() { return s_cache; }

bool is_mounted() { return s_mounted; }

bool is_dirty() {
  // Lock isn't strictly required in P.1 (s_dirty is a single bool
  // mutated nowhere), but reading under the same mutex P.3's setter
  // will use keeps the API contract stable across phases.
  mutex_enter_blocking(&s_mutex);
  const bool d = s_dirty;
  mutex_exit(&s_mutex);
  return d;
}

}  // namespace prefs

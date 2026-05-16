// Persistent user preferences (FR-18) — P.1 skeleton + P.2 boot-restore
// + P.3 wear-protected writeback.
// See include/state/prefs.h for the contract.

#include "prefs.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <pico/mutex.h>

#include <string.h>

namespace prefs {

namespace {

// Defaults applied when no /prefs.json exists (FR-18.5). Theme falls
// back to APOLLO_AMBER per FR-15.2 / plan P.1; matches the explicit
// theme::set() that lived in setup() before this module landed.
constexpr Prefs kDefaults = {
    .schema_v = kSchemaVersion,
    .theme    = theme::Id::APOLLO_AMBER,
};

// In-RAM cache. Read-mostly: only mutated by Core 0 setters (none
// until P.3) under s_mutex. Render-side readers (Core 1) take a
// const reference via current() and copy out anything they need;
// the POD layout means a torn read is impossible without a setter.
Prefs s_cache = kDefaults;

// Guards {s_cache, s_dirty} for P.3's writeback tick — the debounced
// flush will need a coherent snapshot of "what's the current value
// AND is it different from what's on flash". Initialised in begin().
mutex_t s_mutex;

bool s_mounted = false;  // LittleFS.begin() result — gates load + writeback I/O.
bool s_dirty   = false;  // Set by setters; cleared by a successful flush.

// FR-18.4 timing state. Both deadlines are wall-clock millis()
// stamps; the wrap-safe `(int32_t)(now - deadline) >= 0` compare
// keeps them honest across the 49-day rollover (CODING_PRACTICES §2).
uint32_t s_last_setter_ms = 0;  // Most recent set_*() call. Settle = +5 s.
uint32_t s_last_flush_ms  = 0;  // Most recent successful write. Rate-cap = +30 s.
bool     s_ever_flushed   = false;  // Suppress the rate-cap on the very
                                    // first write so a fresh device's
                                    // first theme change persists at
                                    // settle + 0, not settle + 30 s.

// FR-18.4 windows.
constexpr uint32_t kSettleMs   = 5'000;
constexpr uint32_t kRateCapMs  = 30'000;

// Snapshot of what's currently on flash, mirrored in RAM so we can
// suppress no-op writes (FR-18.4 step 2). Initialised from kDefaults
// and overwritten by load() if /prefs.json existed at boot, so the
// first set_theme(restored_theme) is correctly recognised as a no-op.
Prefs s_on_flash = kDefaults;

// Forward-version passthrough buffer (FR-18.5). Populated once by
// load() with any unknown top-level keys from /prefs.json, then
// read-only for the rest of the boot. P.3's write path splices it
// onto the JSON it emits so a v1 firmware re-saving a v2 file does
// not silently drop the v2 keys. 192 B comfortably accommodates the
// foreseeable v2 schema (default scene id, brightness ceiling, mute
// flag, night-threshold override) with headroom; an over-large file
// is logged and the unknown keys are dropped on the floor — better
// than a silent buffer overrun. The leading `,` is part of the
// payload by contract so the writer can splice without conditional
// formatting.
constexpr size_t kPassthroughCap = 192;
char   s_passthrough[kPassthroughCap] = "";
size_t s_passthrough_len               = 0;

constexpr const char* kPath = "/prefs.json";

// Single source of truth for "is this a v1 known key?" — used by both
// the parse step (apply known values) and the passthrough capture
// (everything not in this set is forward-version data we must
// preserve).
bool is_known_key(const char* k) {
  return strcmp(k, "v") == 0 || strcmp(k, "theme") == 0;
}

// Parse /prefs.json into the cache + passthrough buffer. Called once
// from begin() after a successful mount. Per FR-18.5 a missing or
// malformed file is treated as "no prefs yet" — defaults stay in
// place and we do NOT recreate the file (that happens on the first
// setter call in P.3). Logs each outcome so a confusing boot is
// diagnosable from serial alone.
void load() {
  if (!s_mounted) return;

  if (!LittleFS.exists(kPath)) {
    Serial.println("[prefs] no /prefs.json — defaults apply");
    return;
  }

  File f = LittleFS.open(kPath, "r");
  if (!f) {
    Serial.println("[prefs] open /prefs.json FAILED — defaults apply");
    return;
  }
  const size_t sz = f.size();
  // Hard cap matches the parse buffer; a runaway file is a corruption
  // signal, not something to grow into. 256 B leaves room for the v1
  // schema + a healthy chunk of forward-version passthrough.
  constexpr size_t kFileCap = 256;
  if (sz == 0 || sz >= kFileCap) {
    Serial.print("[prefs] /prefs.json bad size=");
    Serial.print(sz);
    Serial.println(" — defaults apply");
    f.close();
    return;
  }
  char buf[kFileCap];
  const size_t n = f.readBytes(buf, sz);
  f.close();
  buf[n] = '\0';

  // Same StaticJsonDocument-with-deprecation-pragma pattern the
  // mqtt_link handlers use (CODING_PRACTICES §1) — JsonDocument's
  // default heap allocator violates NFR-2.2 even though we're in
  // setup() rather than the hot path; static is the consistent
  // choice across the codebase.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<128> doc;
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, n);
  if (err) {
    Serial.print("[prefs] /prefs.json parse FAILED err=");
    Serial.print(err.c_str());
    Serial.println(" — defaults apply");
    return;
  }
  if (!doc.is<JsonObject>()) {
    Serial.println("[prefs] /prefs.json not an object — defaults apply");
    return;
  }

  // Schema gate (FR-18.1). Older versions are accepted at face value
  // (the v1 keys are a subset by definition); newer versions fall
  // through the same applicator but their unknown keys ride the
  // passthrough buffer back to disk so a downgrade is non-destructive.
  const uint8_t v = doc["v"] | 0;
  if (v == 0) {
    Serial.println("[prefs] /prefs.json missing schema version "
                   "— treating as v1");
  } else if (v != kSchemaVersion) {
    Serial.print("[prefs] /prefs.json schema v=");
    Serial.print(v);
    Serial.print(" (firmware v=");
    Serial.print(kSchemaVersion);
    Serial.println(") — applying known keys, preserving the rest");
  }

  // ── Known v1 keys ─────────────────────────────────────────────
  // theme: wire id string (FR-18.2). Unknown id → fall back to
  // default + log; we never want a typo'd file to wedge the boot.
  const char* theme_id = doc["theme"] | static_cast<const char*>(nullptr);
  if (theme_id != nullptr && theme_id[0] != '\0') {
    theme::Id id;
    if (theme::id_from_string(theme_id, &id)) {
      s_cache.theme = id;
      Serial.print("[prefs] restored theme=");
      Serial.println(theme_id);
    } else {
      Serial.print("[prefs] unknown theme id=");
      Serial.print(theme_id);
      Serial.println(" — keeping default");
    }
  }

  // Mirror what we just learned about the on-disk state so the P.3
  // writeback gate can recognise a no-op set (FR-18.4 step 2). We
  // copy s_cache (rather than re-reading individual keys) because
  // any unknown-theme fallback above already settled the cache to
  // the value the file effectively represents.
  s_on_flash = s_cache;

  // ── Forward-version passthrough capture ──────────────────────
  // Walk the parsed object once, serialise each unrecognised
  // top-level entry into s_passthrough as `,"key":<value>` so P.3's
  // writer can splice the buffer onto its own emission without
  // re-parsing. Truncation is logged but not fatal — better to drop
  // forward keys we can't fit than to corrupt the file on rewrite.
  bool truncated = false;
  for (JsonPair kv : doc.as<JsonObject>()) {
    const char* key = kv.key().c_str();
    if (is_known_key(key)) continue;

    // measureJson on a tiny ad-hoc object gives the exact serialised
    // length of `"key":value` so we can size-check before writing.
    // Heap-free: ArduinoJson's measure path is purely arithmetic on
    // the existing variant tree.
    const size_t value_len = measureJson(kv.value());
    const size_t key_len   = strlen(key);
    // `,"key":value` → 1 (`,`) + 2 (quotes) + key + 1 (`:`) + value
    const size_t entry_len = 1 + 2 + key_len + 1 + value_len;
    if (s_passthrough_len + entry_len + 1 > kPassthroughCap) {
      truncated = true;
      break;
    }
    s_passthrough[s_passthrough_len++] = ',';
    s_passthrough[s_passthrough_len++] = '"';
    memcpy(&s_passthrough[s_passthrough_len], key, key_len);
    s_passthrough_len += key_len;
    s_passthrough[s_passthrough_len++] = '"';
    s_passthrough[s_passthrough_len++] = ':';
    s_passthrough_len += serializeJson(kv.value(),
                                       &s_passthrough[s_passthrough_len],
                                       kPassthroughCap - s_passthrough_len);
  }
  s_passthrough[s_passthrough_len] = '\0';

  if (truncated) {
    Serial.println("[prefs] passthrough buffer full — some forward-version "
                   "keys will be dropped on next write");
  } else if (s_passthrough_len > 0) {
    Serial.print("[prefs] preserving ");
    Serial.print(s_passthrough_len);
    Serial.println(" B of forward-version keys");
  }
}

constexpr const char* kTmpPath = "/prefs.json.tmp";

// Atomic-write /prefs.json from the current cache snapshot
// (FR-18.4). Builds the JSON in a stack buffer, writes to a temp
// file, fsyncs via close(), then renames over the canonical path.
// Returns true on success — caller updates s_on_flash + s_last_flush_ms.
//
// Snapshot semantics: caller MUST hold s_mutex while reading the
// cache into a local Prefs and computing dirty, but releases the
// mutex before calling write_atomic() to keep the held window short.
// A concurrent setter between snapshot and rename is fine — it will
// just re-arm the dirty flag and the next tick() will re-flush.
bool write_atomic(const Prefs& snapshot) {
  if (!s_mounted) return false;

  // 256 B fits the v1 schema (~64 B) plus the full passthrough
  // buffer (192 B) + structural overhead. Same hard cap as load().
  constexpr size_t kFileCap = 256;
  char buf[kFileCap];
  const char* theme_id = theme::string_from_id(snapshot.theme);
  // Format: {"v":1,"theme":"<id>"<passthrough>}
  // s_passthrough either is empty or starts with `,` so it splices
  // cleanly after the theme field with no conditional formatting.
  const int n = snprintf(buf, sizeof(buf),
                         "{\"v\":%u,\"theme\":\"%s\"%s}",
                         static_cast<unsigned>(kSchemaVersion),
                         theme_id,
                         s_passthrough);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
    Serial.print("[prefs] write FAILED — payload would be ");
    Serial.print(n);
    Serial.println(" B");
    return false;
  }

  // Open the temp path fresh ("w" truncates) so a stale half-written
  // tmp from a prior crash doesn't leak into the new payload.
  File f = LittleFS.open(kTmpPath, "w");
  if (!f) {
    Serial.println("[prefs] write FAILED — open tmp");
    return false;
  }
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(buf),
                                 static_cast<size_t>(n));
  // close() flushes; LittleFS doesn't expose a separate fsync().
  f.close();
  if (written != static_cast<size_t>(n)) {
    Serial.print("[prefs] write FAILED — short write ");
    Serial.print(written);
    Serial.print("/");
    Serial.println(n);
    LittleFS.remove(kTmpPath);
    return false;
  }

  // Atomic publish (FR-18.4). LittleFS::rename() overwrites the
  // destination if it exists, which is exactly the semantics we
  // want — the new file becomes /prefs.json in one step or not at
  // all from a power-loss perspective.
  if (!LittleFS.rename(kTmpPath, kPath)) {
    Serial.println("[prefs] write FAILED — rename");
    LittleFS.remove(kTmpPath);
    return false;
  }
  return true;
}

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
    // P.2: try to restore from /prefs.json. Failures degrade to
    // defaults — never crash, never wedge the boot.
    load();
    // Clean up any orphaned tmp from a crash mid-write on the
    // previous boot — write_atomic() handles its own cleanup on
    // failure, but a hard power-loss between open() and rename()
    // can leave one behind. Cheap: a remove() on a missing path
    // is a no-op + one stat lookup.
    if (LittleFS.exists(kTmpPath)) {
      LittleFS.remove(kTmpPath);
    }
  } else {
    // Treat as "no prefs yet" — defaults apply (FR-18.5). Don't
    // retry; the most likely cause is a zero-sized partition (config
    // error) and burning watchdog budget on retries won't help.
    Serial.println("[prefs] LittleFS mount failed — defaults apply, "
                   "writeback disabled");
  }
}

void set_theme(theme::Id id) {
  // Drive the visible swap first so callers see the same FR-15.4
  // next-frame contract they would have got from the bare theme::
  // API. theme::set() is itself idempotent (no-op when id is
  // unchanged) so this is safe to call unconditionally.
  theme::set(id);

  mutex_enter_blocking(&s_mutex);
  if (s_cache.theme != id) {
    s_cache.theme    = id;
    s_dirty          = true;
    s_last_setter_ms = millis();
  }
  mutex_exit(&s_mutex);
}

void cycle_theme(int8_t delta) {
  // Mirror theme::cycle()'s wrap math so the persisted-and-non-
  // persisted code paths walk the enum in lockstep.
  const uint8_t n   = static_cast<uint8_t>(theme::Id::COUNT);
  if (n == 0) return;
  const uint8_t cur = static_cast<uint8_t>(theme::current());
  const int     step = delta % static_cast<int>(n);
  const uint8_t nxt = static_cast<uint8_t>((cur + n + step) % n);
  set_theme(static_cast<theme::Id>(nxt));
}

void tick(uint32_t now_ms) {
  // Cheap fast path: most ticks find nothing to do. One byte read
  // outside the mutex is fine — a stale `false` just defers work
  // one iteration; a stale `true` falls through to the strict
  // re-check inside the mutex. No correctness risk.
  if (!s_dirty || !s_mounted) return;

  // Snapshot under the mutex so a concurrent setter can't race the
  // rate-cap / settle math. Held window: a few word copies.
  Prefs    snapshot;
  uint32_t last_setter_ms;
  bool     dirty;
  mutex_enter_blocking(&s_mutex);
  snapshot       = s_cache;
  last_setter_ms = s_last_setter_ms;
  dirty          = s_dirty;
  mutex_exit(&s_mutex);
  if (!dirty) return;

  // Settle window — wait for the user to stop fiddling (FR-18.4 #1).
  // Wrap-safe compare per CODING_PRACTICES §2.
  if (static_cast<int32_t>(now_ms - (last_setter_ms + kSettleMs)) < 0) return;

  // No-op suppression (FR-18.4 #2). The user cycled themes and
  // landed back on the persisted value — clear dirty without
  // touching flash.
  if (memcmp(&snapshot, &s_on_flash, sizeof(Prefs)) == 0) {
    mutex_enter_blocking(&s_mutex);
    // Only clear if the cache still matches the snapshot we
    // checked — a concurrent set_theme() during write would have
    // re-armed dirty and we must preserve it.
    if (memcmp(&s_cache, &snapshot, sizeof(Prefs)) == 0) {
      s_dirty = false;
    }
    mutex_exit(&s_mutex);
    return;
  }

  // Rate cap (FR-18.4 #3) — at most one flush per 30 s once we've
  // ever written. The first write skips the cap so a fresh device's
  // first theme change persists at settle + 0 (better UX, same wear
  // budget over the device lifetime).
  if (s_ever_flushed &&
      static_cast<int32_t>(now_ms - (s_last_flush_ms + kRateCapMs)) < 0) {
    return;
  }

  if (write_atomic(snapshot)) {
    s_last_flush_ms = now_ms;
    s_ever_flushed  = true;
    Serial.print("[prefs] flushed theme=");
    Serial.println(theme::string_from_id(snapshot.theme));

    mutex_enter_blocking(&s_mutex);
    s_on_flash = snapshot;
    // Same conditional-clear as the no-op path: if the user changed
    // their mind during the write, leave dirty set so the next
    // tick re-flushes.
    if (memcmp(&s_cache, &snapshot, sizeof(Prefs)) == 0) {
      s_dirty = false;
    }
    mutex_exit(&s_mutex);
  }
  // On failure write_atomic() already logged + cleaned up. Leave
  // dirty set so the next tick (≥ 30 s away under the rate cap, or
  // immediately on a fresh device) retries — flash hiccups should
  // be rare, and we already paid the wear of one failed sector.
}

const Prefs& current() { return s_cache; }

bool is_mounted() { return s_mounted; }

bool is_dirty() {
  // Lock isn't strictly required in P.2 (s_dirty is a single bool
  // mutated nowhere yet), but reading under the same mutex P.3's
  // setter will use keeps the API contract stable across phases.
  mutex_enter_blocking(&s_mutex);
  const bool d = s_dirty;
  mutex_exit(&s_mutex);
  return d;
}

const char* unknown_passthrough() { return s_passthrough; }

void reset() {
  // FR-18.8 — wipe the persisted prefs file and disarm the writeback
  // pipeline so the imminent reboot sees "no prefs yet" and applies
  // defaults via FR-18.5. We deliberately do NOT touch s_cache: the
  // caller (mqtt_link's reset handler) follows this up immediately
  // with rp2040.reboot(), so no UI ever observes the in-RAM state
  // between the wipe and the restart. Clearing the dirty flag (and
  // s_on_flash) prevents a stray tick() between this call and the
  // reboot from re-emitting the file we just deleted.
  mutex_enter_blocking(&s_mutex);
  s_dirty = false;
  if (s_mounted) {
    if (LittleFS.exists(kPath)) {
      if (LittleFS.remove(kPath)) {
        Serial.println("[prefs] reset: /prefs.json removed");
      } else {
        Serial.println("[prefs] reset: remove FAILED");
      }
    } else {
      Serial.println("[prefs] reset: no /prefs.json to remove");
    }
    // Tmp from a crash mid-write would otherwise survive a reset
    // and get rename()'d into place on the next setter \u2014 mop it up.
    if (LittleFS.exists(kTmpPath)) {
      LittleFS.remove(kTmpPath);
    }
  } else {
    Serial.println("[prefs] reset: filesystem not mounted; defaults already in effect");
  }
  // Re-mirror s_on_flash to defaults so the post-reboot path doesn't
  // matter \u2014 if the caller skips reboot(), the next setter still
  // behaves correctly (writes when value differs from defaults).
  s_on_flash = kDefaults;
  mutex_exit(&s_mutex);
}

}  // namespace prefs

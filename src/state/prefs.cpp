// Persistent user preferences (FR-18) — P.1 skeleton + P.2 boot-restore.
// See include/state/prefs.h for the contract. P.3 adds the debounced
// wear-protected writeback.

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
bool s_dirty   = false;  // Always false in P.2; P.3 flips it on setter.

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
  } else {
    // Treat as "no prefs yet" — defaults apply (FR-18.5). Don't
    // retry; the most likely cause is a zero-sized partition (config
    // error) and burning watchdog budget on retries won't help.
    Serial.println("[prefs] LittleFS mount failed — defaults apply, "
                   "writeback disabled");
  }
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

}  // namespace prefs

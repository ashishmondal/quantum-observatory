// Implementation of include/net/ha_discovery.h (FR-20 / phase HA.2).
//
// One retained config per entity, published synchronously on Core 0
// from inside mqtt_link's CONNECTING success branch. snprintf builds
// each payload into a stack buffer — no ArduinoJson dependency, no
// heap, no shared scratch. Per-entity payload tops out around 420 B
// (longest is the float-heap sensor with the full device block);
// kPayloadCapacity = 512 leaves NFR-2.3-style headroom and fits
// comfortably inside the existing PubSubClient buffer
// (mqtt_link::kPubSubBufferSize = 832).
//
// The entity table is the single source of truth for the FR-20.5 v1
// surface. Adding a new sensor here is a two-line diff:
//   1. add a row,
//   2. add the matching field to publish_status() in mqtt_link.cpp.
// Document the new field in docs/MQTT_TOPICS.md in the same commit.

#include "ha_discovery.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <stdio.h>

#include "config.h"
#include "secrets.h"

namespace ha_discovery {

namespace {

// One row per HA entity. Lifetime is static-storage (all const char*
// point into .rodata) so the struct stays POD and the table compiles
// straight into flash. Fields with no value pass nullptr — omitted
// from the JSON entirely (HA defaults are usually what we want).
struct EntityDecl {
  const char* component;        // "sensor" or "binary_sensor"
  const char* object_id;        // unique within the device (no slashes)
  const char* name;             // human-readable, prefixed by device name in UI
  const char* value_template;   // Jinja against observatory/status JSON
  const char* unit;             // unit_of_measurement; nullptr to omit
  const char* device_class;     // HA device_class; nullptr to omit
  const char* state_class;      // measurement / total_increasing / nullptr
  const char* entity_category;  // "diagnostic" or nullptr
  const char* icon;             // mdi:foo or nullptr
  // binary_sensor only — payload values for ON/OFF. value_template
  // already emits "ON"/"OFF" strings for our binary sensors, so these
  // can be nullptr to use HA's defaults of "ON"/"OFF".
};

// Order is purely cosmetic (HA sorts alphabetically by name in the
// device card anyway). Kept grouped by category to make the table
// scannable at a glance: scene, render diagnostics, sensors,
// link diagnostics.
constexpr EntityDecl kEntities[] = {
  // ── scene / prefs ────────────────────────────────────────────────
  { "sensor",        "scene",
    "Scene",
    "{{ value_json.scene_id }}",
    nullptr, nullptr, nullptr, nullptr, "mdi:movie-open" },
  { "binary_sensor", "prefs_dirty",
    "Prefs dirty",
    "{{ 'ON' if value_json.prefs_dirty else 'OFF' }}",
    nullptr, "problem", nullptr, "diagnostic", nullptr },

  // ── render diagnostics ───────────────────────────────────────────
  { "sensor",        "fps",
    "Render FPS",
    "{{ value_json.fps }}",
    "fps", nullptr, "measurement", "diagnostic", "mdi:speedometer" },
  { "sensor",        "render_slack",
    "Render slack",
    "{{ value_json.render_slack_ms }}",
    "ms", nullptr, "measurement", "diagnostic", "mdi:timer-sand" },

  // ── system diagnostics ───────────────────────────────────────────
  { "sensor",        "uptime",
    "Uptime",
    "{{ value_json.uptime_s }}",
    "s", "duration", "total_increasing", "diagnostic", nullptr },
  { "sensor",        "free_heap",
    "Free heap",
    "{{ value_json.free_heap }}",
    "B", "data_size", "measurement", "diagnostic", "mdi:memory" },
  { "sensor",        "rssi",
    "Wi-Fi RSSI",
    "{{ value_json.rssi }}",
    "dBm", "signal_strength", "measurement", "diagnostic", nullptr },

  // ── sensor surface (FR-7) ────────────────────────────────────────
  { "sensor",        "temperature",
    "Enclosure temperature",
    // The status payload writes JSON null until the first DS3231 read
    // succeeds; the `is none` guard keeps HA from logging template
    // errors during that boot window.
    "{{ value_json.temp_c if value_json.temp_c is not none else 'unknown' }}",
    "\u00b0C", "temperature", "measurement", nullptr, nullptr },
  { "sensor",        "light_raw",
    "Ambient light (raw)",
    "{{ value_json.light_raw }}",
    nullptr, nullptr, "measurement", "diagnostic", "mdi:brightness-5" },
  { "binary_sensor", "night",
    "Night mode",
    "{{ 'ON' if value_json.night else 'OFF' }}",
    nullptr, "light", nullptr, nullptr, nullptr },
  { "binary_sensor", "thermal_hot",
    "Thermal-safe active",
    "{{ 'ON' if value_json.hot else 'OFF' }}",
    nullptr, "heat", nullptr, "diagnostic", nullptr },

  // ── MQTT link diagnostics ────────────────────────────────────────
  { "sensor",        "mqtt_state",
    "MQTT state",
    "{{ value_json.mqtt_state }}",
    nullptr, nullptr, nullptr, "diagnostic", "mdi:lan-connect" },
  { "sensor",        "mqtt_rc",
    "MQTT last rc",
    "{{ value_json.mqtt_rc }}",
    nullptr, nullptr, nullptr, "diagnostic", nullptr },
  { "sensor",        "mqtt_backoff",
    "MQTT backoff",
    "{{ value_json.mqtt_backoff_s }}",
    "s", "duration", "measurement", "diagnostic", "mdi:timer-outline" },
};
constexpr size_t kEntityCount = sizeof(kEntities) / sizeof(kEntities[0]);

constexpr size_t kPayloadCapacity = 512;
constexpr size_t kTopicCapacity   = 96;

// Helper: append `key: "value"` to `buf` only when `value` is non-null.
// Returns the new write position; on overflow leaves buf unchanged
// (n is the remaining capacity, returned unmodified). All discovery
// payloads are small enough that overflow is a programming bug, but
// snprintf's truncation semantics keep us crash-safe regardless.
int append_kv_str(char* buf, int n, const char* key, const char* value) {
  if (value == nullptr) return n;
  // Each call writes `, "key":"value"` — the leading comma is fine
  // because the first key in the payload is the always-present
  // "name" emitted by the main snprintf.
  const int w = snprintf(buf, n, ",\"%s\":\"%s\"", key, value);
  if (w <= 0 || w >= n) return n;  // overflow: best-effort, drop the field
  return n - w;
}

}  // namespace

void publish_all(PubSubClient& client) {
  // Resolve the device IP once — used for `configuration_url` so a
  // click on the HA device card opens this device's web UI (or, in
  // our case, just the bare IP, which is harmless and tells the
  // operator where to point a browser).
  const IPAddress ip = WiFi.localIP();
  char ip_str[16];
  snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
           ip[0], ip[1], ip[2], ip[3]);

  size_t ok_count = 0;
  for (size_t i = 0; i < kEntityCount; ++i) {
    const EntityDecl& e = kEntities[i];

    char topic[kTopicCapacity];
    const int tn = snprintf(topic, sizeof(topic),
                            "%s/%s/%s/%s/config",
                            HA_DISCOVERY_PREFIX, e.component,
                            MQTT_CLIENT_ID, e.object_id);
    if (tn <= 0 || tn >= static_cast<int>(sizeof(topic))) {
      Serial.print("[ha-disc] topic overflow obj=");
      Serial.println(e.object_id);
      continue;
    }

    // Payload: required keys first, then optional kv pairs, then
    // the device + availability blocks. Build with one big snprintf
    // for the required core so the format string is auditable in
    // one place; append optionals through append_kv_str.
    char payload[kPayloadCapacity];
    int written = snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\","
         "\"unique_id\":\"%s_%s\","
         "\"state_topic\":\"observatory/status\","
         "\"value_template\":\"%s\","
         "\"availability_topic\":\"observatory/availability\","
         "\"payload_available\":\"online\","
         "\"payload_not_available\":\"offline\","
         "\"device\":{"
           "\"identifiers\":[\"%s\"],"
           "\"name\":\"Quantum Observatory\","
           "\"manufacturer\":\"DIY\","
           "\"model\":\"RP2040 + 64x32 HUB75\","
           "\"sw_version\":\"%s\","
           "\"configuration_url\":\"http://%s\""
         "}",
        e.name,
        MQTT_CLIENT_ID, e.object_id,
        e.value_template,
        MQTT_CLIENT_ID,
        FW_VERSION,
        ip_str);
    if (written <= 0 || written >= static_cast<int>(sizeof(payload))) {
      Serial.print("[ha-disc] payload overflow obj=");
      Serial.println(e.object_id);
      continue;
    }

    // Append optionals. Each helper call writes `,"k":"v"` so the
    // running JSON is always valid object-mid-state. `pos` is the
    // current write offset; `remaining` is what's left including the
    // trailing NUL byte.
    int pos = written;
    auto append = [&](const char* k, const char* v) {
      const int remaining = static_cast<int>(sizeof(payload)) - pos;
      if (remaining <= 0) return;
      const int n = append_kv_str(payload + pos, remaining, k, v);
      // append_kv_str returns the NEW remaining count (== old remaining
      // when it skipped or overflowed; otherwise less). Convert back
      // to bytes-written for the write cursor.
      pos += (remaining - n);
    };
    append("unit_of_measurement", e.unit);
    append("device_class",        e.device_class);
    append("state_class",         e.state_class);
    append("entity_category",     e.entity_category);
    append("icon",                e.icon);
    written = pos;

    // Close the object.
    if (written + 2 >= static_cast<int>(sizeof(payload))) {
      Serial.print("[ha-disc] close-brace overflow obj=");
      Serial.println(e.object_id);
      continue;
    }
    payload[written++] = '}';
    payload[written]   = '\0';

    // Retain so HA's MQTT integration recovers the entity set on
    // its own restart without waiting for our next reconnect. qos:0
    // is fine: configs are idempotent — a missed config on this
    // session just means HA will re-register on the next.
    const bool pub_ok = client.publish(topic, payload, /*retain=*/true);
    if (pub_ok) {
      ++ok_count;
      Serial.print("[ha-disc] pub ");
      Serial.print(topic);
      Serial.print(' ');
      Serial.print(written);
      Serial.println("B");
    } else {
      Serial.print("[ha-disc] pub FAILED ");
      Serial.println(topic);
    }
  }
  Serial.print("[ha-disc] published ");
  Serial.print(ok_count);
  Serial.print('/');
  Serial.print(kEntityCount);
  Serial.println(" entity configs");
}

}  // namespace ha_discovery

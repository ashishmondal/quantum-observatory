// Implementation of include/mqtt_link.h.
//
// PubSubClient is the de-facto Arduino MQTT client. We hand it our
// own WiFiClient and let it own the TCP socket; we drive its loop()
// from poll() and check connected() to detect outages.
//
// Static-only buffers throughout (NFR-2.2). The status heartbeat
// payload is serialised by ArduinoJson into a fixed StaticJsonDocument
// sized per NFR-2.3 (largest documented payload + 25%).

#include "mqtt_link.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "scene_state.h"
#include "secrets.h"
#include "light_sensor.h"
#include "thermal_monitor.h"
#include "time_of_day.h"
#include "wifi_link.h"

namespace mqtt_link {

namespace {

constexpr uint32_t kBackoffStartMs    = 1000u;     // FR-5.2 floor
constexpr uint32_t kBackoffMaxMs      = 60000u;    // FR-5.2 ceiling
constexpr uint32_t kStatusIntervalMs  = 30000u;    // §5.4 heartbeat cadence

constexpr const char* kTopicStatus  = "observatory/status";
constexpr const char* kTopicScene   = "observatory/scene";   // FR-1.1
constexpr const char* kTopicClear   = "observatory/clear_sticky"; // §5.3 / FR-2.2
constexpr const char* kTopicNight   = "observatory/night";   // FR-7.4
constexpr const char* kTopicThermal = "observatory/thermal"; // FR-7.4
constexpr const char* kTopicTime    = "observatory/time";    // FR-9.5

// §5.4 example payload is ~85 bytes serialised. NFR-2.3 → max + 25%.
// Using 256 here gives generous headroom for future fields without
// wasting much SRAM.
constexpr size_t kStatusJsonCapacity = 256;

// §5.1 example payload is ~150 bytes. The `overrides.text` field is
// the largest variable contributor — Director-side truncation caps it
// at ~14 chars (FR-4.4), so payloads stay well under 256 B in practice.
// 384 = max documented + ~150% headroom (NFR-2.3) and rounds to a tidy
// PubSubClient buffer when added to topic + framing below.
constexpr size_t kSceneJsonCapacity = 384;

// PubSubClient inbound/outbound share a single buffer. Must be ≥ the
// largest payload + topic + a few bytes of MQTT framing. Sized to the
// scene topic since that's bigger than the status payload.
constexpr size_t kPubSubBufferSize = kSceneJsonCapacity + 64;

WiFiClient   s_tcp;
PubSubClient s_client(s_tcp);

State    s_state            = State::IDLE;
uint32_t s_next_attempt_ms  = 0;
uint32_t s_backoff_ms       = kBackoffStartMs;
uint32_t s_last_status_ms   = 0;
uint32_t s_status_publishes = 0;
uint32_t s_scene_msgs       = 0;
uint32_t s_scene_rejects    = 0;
uint32_t s_threshold_msgs    = 0;  // night + thermal combined
uint32_t s_threshold_rejects = 0;
uint32_t s_time_msgs         = 0;
uint32_t s_time_rejects      = 0;
uint32_t s_clear_msgs        = 0;

// Tag identifying which §5.2 threshold topic a payload arrived on.
// Drives the right setter + range validation in handle_thresholds().
enum class ThresholdKind : uint8_t { NIGHT, THERMAL };

// §5.2 threshold-payload handler — shared between observatory/night
// (FR-7.4 LDR) and observatory/thermal (FR-7.4 DS3231). Same
// validation discipline as the scene handler (FR-1.3 / FR-1.4):
// oversize → drop, parse failure → drop, missing/out-of-range field
// → drop, never crash. Range checks are tighter than the JSON types
// permit — we'd rather reject `{"threshold":-1}` than feed it to a
// uint16_t setter. (added in phase 5.5.3)
void handle_thresholds(ThresholdKind kind, char* buf, unsigned int length) {
  ++s_threshold_msgs;
  const char* tag = (kind == ThresholdKind::NIGHT) ? "night" : "thermal";

  // ArduinoJson v7: see on_mqtt_message() for the heap-vs-static
  // rationale (NFR-2.2).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<128> doc;  // §5.2 payload is ~40 B; 128 leaves comfortable headroom
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, length);
  if (err) {
    ++s_threshold_rejects;
    Serial.print("[mqtt] ");
    Serial.print(tag);
    Serial.print(" parse FAILED err=");
    Serial.print(err.c_str());
    Serial.print(" payload=");
    Serial.println(buf);
    return;
  }

  // Both fields required — without them there's nothing to apply.
  // Use `.is<int>()` rather than the `| default` shorthand so we can
  // distinguish "missing" from "zero" (zero is a legal, if useless,
  // hysteresis).
  if (!doc["threshold"].is<int>() || !doc["hysteresis"].is<int>()) {
    ++s_threshold_rejects;
    Serial.print("[mqtt] ");
    Serial.print(tag);
    Serial.print(" missing fields payload=");
    Serial.println(buf);
    return;
  }
  const int threshold  = doc["threshold"].as<int>();
  const int hysteresis = doc["hysteresis"].as<int>();

  if (kind == ThresholdKind::NIGHT) {
    // 12-bit ADC → both values must fit 0..4095. Hysteresis MUST be
    // strictly positive; 0 collapses the Schmitt to a single edge
    // and chatters at the boundary.
    if (threshold < 0 || threshold > 4095
     || hysteresis <= 0 || hysteresis > 4095) {
      ++s_threshold_rejects;
      Serial.print("[mqtt] night out-of-range threshold=");
      Serial.print(threshold);
      Serial.print(" hysteresis=");
      Serial.println(hysteresis);
      return;
    }
    light_sensor::set_thresholds(static_cast<uint16_t>(threshold),
                                 static_cast<uint16_t>(hysteresis));
  } else {
    // Signed °C, plenty of slack on either side of any plausible
    // ambient. DS3231 spec range is −55..+125; we narrow to int8_t
    // bounds and require positive hysteresis (same reason as above).
    if (threshold < -40 || threshold > 125
     || hysteresis <= 0 || hysteresis > 50) {
      ++s_threshold_rejects;
      Serial.print("[mqtt] thermal out-of-range threshold=");
      Serial.print(threshold);
      Serial.print(" hysteresis=");
      Serial.println(hysteresis);
      return;
    }
    thermal_monitor::set_thresholds(static_cast<int8_t>(threshold),
                                    static_cast<int8_t>(hysteresis));
  }
}

// observatory/time handler — FR-9.5 RTC correction path. Payload:
//   {"epoch_utc": 1714608000, "tz_offset_min": 330}
// epoch_utc is Unix seconds (UTC); tz_offset_min is local-UTC in
// minutes (e.g. +330 for IST, -480 for PST). The DS3231 stores LOCAL
// time per the Phase 3.6.3 decision; tod::set_from_mqtt does the
// conversion + RTC write + re-poll. Validation discipline matches the
// other inbound handlers (FR-1.3 / FR-1.4): malformed → log + drop.
// Range checks: epoch must be plausible (after 2020-01-01 to keep the
// chip's two-digit year happy), tz must be in the valid IANA range
// of −12:00..+14:00.
void handle_time(char* buf, unsigned int length, uint32_t now_ms) {
  ++s_time_msgs;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<128> doc;
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, length);
  if (err) {
    ++s_time_rejects;
    Serial.print("[mqtt] time parse FAILED err=");
    Serial.print(err.c_str());
    Serial.print(" payload=");
    Serial.println(buf);
    return;
  }

  // epoch_utc requires a 32-bit field; ArduinoJson's `is<long>()`
  // covers that on every Arduino target. tz fits comfortably in int.
  if (!doc["epoch_utc"].is<long>() || !doc["tz_offset_min"].is<int>()) {
    ++s_time_rejects;
    Serial.print("[mqtt] time missing fields payload=");
    Serial.println(buf);
    return;
  }
  const long epoch_utc     = doc["epoch_utc"].as<long>();
  const int  tz_offset_min = doc["tz_offset_min"].as<int>();

  // 2020-01-01 00:00:00 UTC = 1577836800. The DS3231 BCD year field
  // is two digits (2000..2099); reject anything that would underflow
  // the chip on write rather than letting ds3231::write() fail
  // silently. Upper bound is generous (year 2099-ish).
  constexpr long kMinEpoch = 1577836800L;
  constexpr long kMaxEpoch = 4102444800L;  // 2100-01-01
  if (epoch_utc < kMinEpoch || epoch_utc >= kMaxEpoch) {
    ++s_time_rejects;
    Serial.print("[mqtt] time out-of-range epoch_utc=");
    Serial.println(epoch_utc);
    return;
  }
  // IANA tz range (−12:00..+14:00); allow a bit of slack for half/
  // quarter-hour zones already inside that envelope.
  if (tz_offset_min < -720 || tz_offset_min > 840) {
    ++s_time_rejects;
    Serial.print("[mqtt] time out-of-range tz_offset_min=");
    Serial.println(tz_offset_min);
    return;
  }

  // tod::set_from_mqtt writes the RTC, clears the OSF, and forces a
  // re-poll so the next tod::now() reflects the correction. FR-9.5:
  // MQTT is the *correction* path, never the read path — readers
  // still come through tod::now() against the RTC cache.
  if (!tod::set_from_mqtt(static_cast<int32_t>(epoch_utc),
                          static_cast<int16_t>(tz_offset_min), now_ms)) {
    ++s_time_rejects;
    Serial.print("[mqtt] time write FAILED epoch_utc=");
    Serial.print(epoch_utc);
    Serial.print(" tz=");
    Serial.println(tz_offset_min);
    return;
  }
  Serial.print("[mqtt] time applied epoch_utc=");
  Serial.print(epoch_utc);
  Serial.print(" tz=");
  Serial.println(tz_offset_min);
}

// PubSubClient inbound callback. Runs on Core 0 from inside
// PubSubClient::loop() (called from poll()) — same thread as the rest
// of mqtt_link, so no locking needed against our own static state.
// Validation rules (FR-1.3, FR-1.4): malformed JSON or missing
// scene_id is logged and dropped; we never crash and never propagate
// to the renderer. Phase 5.4 wires scene_id → scene_state::request();
// 5.5.3 added the night/thermal threshold dispatch.
void on_mqtt_message(char* topic, uint8_t* payload, unsigned int length) {
  // Common buffer-copy step: every topic we handle is JSON, every
  // handler wants a NUL-terminated C-string ≤ kSceneJsonCapacity.
  // Done once here so the per-topic handlers stay focused on
  // validation/dispatch.
  if (length >= kSceneJsonCapacity) {
    Serial.print("[mqtt] oversize topic=");
    Serial.print(topic);
    Serial.print(" len=");
    Serial.println(length);
    return;
  }
  char buf[kSceneJsonCapacity];
  memcpy(buf, payload, length);
  buf[length] = '\0';

  if (strcmp(topic, kTopicNight) == 0) {
    handle_thresholds(ThresholdKind::NIGHT, buf, length);
    return;
  }
  if (strcmp(topic, kTopicThermal) == 0) {
    handle_thresholds(ThresholdKind::THERMAL, buf, length);
    return;
  }
  if (strcmp(topic, kTopicTime) == 0) {
    // millis() inside the callback is fine — the callback runs from
    // PubSubClient::loop() on Core 0, the same thread that owns tod's
    // smoothing baseline.
    handle_time(buf, length, millis());
    return;
  }
  if (strcmp(topic, kTopicClear) == 0) {
    // §5.3: payload is empty by spec. Don't validate it — a non-empty
    // payload is harmless noise and rejecting it would just give the
    // Director a footgun. clear_sticky() is itself a no-op when no
    // sticky scene is active (FR-2.2).
    ++s_clear_msgs;
    scene_state::clear_sticky();
    Serial.println("[mqtt] clear_sticky");
    return;
  }
  if (strcmp(topic, kTopicScene) != 0) {
    return;  // defensive; we only subscribed to the topics above
  }
  ++s_scene_msgs;

  // ArduinoJson v7 deprecated StaticJsonDocument in favour of
  // JsonDocument — but JsonDocument's default allocator uses
  // malloc/free, which violates NFR-2.2 (no heap on hot paths). The
  // deprecated class is exactly the static-buffer behaviour we want,
  // so silence the warning rather than swap to a heap doc.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<kSceneJsonCapacity> doc;
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, length);
  if (err) {
    ++s_scene_rejects;
    Serial.print("[mqtt] scene parse FAILED err=");
    Serial.print(err.c_str());
    Serial.print(" payload=");
    Serial.println(buf);
    return;  // FR-1.4: malformed JSON does not interrupt active scene
  }

  const char* scene_id = doc["scene_id"] | static_cast<const char*>(nullptr);
  if (scene_id == nullptr || scene_id[0] == '\0') {
    ++s_scene_rejects;
    Serial.print("[mqtt] scene missing scene_id payload=");
    Serial.println(buf);
    return;  // FR-1.3 spirit: nothing to dispatch on
  }

  // §5.1 optional fields — surface in the log so the wire contract is
  // observable end-to-end before 5.4 acts on them.
  const int  priority = doc["priority"] | 1;
  const int  duration = doc["duration"] | 30;
  const bool sticky   = doc["sticky"]   | false;

  // Phase 5.4: resolve to a SceneId and hand to the cross-core
  // dispatcher. Unknown ids are logged and dropped (FR-1.3) — the
  // active scene keeps rendering. Phase 6.1 enforces FR-2.1 priority
  // preemption; Phase 6.2 wires duration/sticky into the lifecycle.
  scene_state::SceneId id;
  if (!scene_state::id_from_string(scene_id, &id)) {
    ++s_scene_rejects;
    Serial.print("[mqtt] scene unknown id=");
    Serial.print(scene_id);
    Serial.print(" prio=");
    Serial.print(priority);
    Serial.print(" dur=");
    Serial.print(duration);
    Serial.print(" sticky=");
    Serial.println(sticky ? 1 : 0);
    return;
  }
  // Clamp priority to the FR-2.1 range (0..5) before handing off;
  // negatives become 0 (lowest), >5 becomes 5 (highest). The Director
  // shouldn't send out-of-range values but FR-1.3 says we don't trust
  // the wire.
  uint8_t prio_u8;
  if      (priority < 0) prio_u8 = 0;
  else if (priority > 5) prio_u8 = 5;
  else                   prio_u8 = static_cast<uint8_t>(priority);
  // Same defensive clamp on duration: negative → default, >3600 →
  // capped (the hard TTL is 1 h anyway, FR-2.4).
  uint16_t dur_u16;
  if      (duration <= 0)   dur_u16 = 30;     // FR-2.3 default
  else if (duration > 3600) dur_u16 = 3600;
  else                      dur_u16 = static_cast<uint16_t>(duration);

  const bool accepted = scene_state::request(id, prio_u8, dur_u16, sticky);
  if (!accepted) {
    ++s_scene_rejects;
    Serial.print("[mqtt] scene preempted id=");
    Serial.print(scene_id);
    Serial.print(" prio=");
    Serial.println(prio_u8);
    return;
  }

  Serial.print("[mqtt] scene id=");
  Serial.print(scene_id);
  Serial.print(" prio=");
  Serial.print(prio_u8);
  Serial.print(" dur=");
  Serial.print(dur_u16);
  Serial.print(" sticky=");
  Serial.println(sticky ? 1 : 0);
}

void log_session_info(const char* event) {
  // One serial line per event (§7 logging). Includes everything a
  // listener on the broker side would need to find this device:
  //   - device IP (the source the broker sees)
  //   - broker host/port (where to subscribe)
  //   - client_id (filterable in the broker)
  //   - heartbeat topic (what to subscribe to)
  Serial.print("[mqtt] ");
  Serial.print(event);
  Serial.print(" device_ip=");
  Serial.print(WiFi.localIP());
  Serial.print(" broker=");
  Serial.print(MQTT_HOST);
  Serial.print(':');
  Serial.print(MQTT_PORT);
  Serial.print(" client_id=");
  Serial.print(MQTT_CLIENT_ID);
  Serial.print(" topic=");
  Serial.println(kTopicStatus);
}

void schedule_retry(uint32_t now_ms, const char* reason) {
  Serial.print("[mqtt] ");
  Serial.print(reason);
  Serial.print(" rc=");
  Serial.print(s_client.state());
  Serial.print(" — retry in ");
  Serial.print(s_backoff_ms / 1000u);
  Serial.println("s");
  s_state = State::DISCONNECTED;
  s_next_attempt_ms = now_ms + s_backoff_ms;
  uint32_t doubled = s_backoff_ms * 2u;
  if (doubled > kBackoffMaxMs) doubled = kBackoffMaxMs;
  s_backoff_ms = doubled;
}

bool publish_status(uint32_t now_ms) {
  // See on_mqtt_message() for why we keep StaticJsonDocument over
  // ArduinoJson v7's heap-backed JsonDocument (NFR-2.2).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<kStatusJsonCapacity> doc;
#pragma GCC diagnostic pop
  doc["scene_id"]  = "n/a";  // TODO(P6): populate from scene_state::current()
  doc["fps"]       = 0;       // TODO(P6): cross-core FPS report from Core 1
  doc["rssi"]      = WiFi.RSSI();
  doc["uptime_s"]  = static_cast<uint32_t>(now_ms / 1000u);
  doc["free_heap"] = static_cast<uint32_t>(rp2040.getFreeHeap());

  char payload[kStatusJsonCapacity];
  const size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload)) {
    Serial.println("[mqtt] status: serialise overflow");
    return false;
  }
  const bool ok = s_client.publish(kTopicStatus, payload);
  if (ok) {
    ++s_status_publishes;
    Serial.print("[mqtt] pub ");
    Serial.print(kTopicStatus);
    Serial.print(' ');
    Serial.print(n);
    Serial.print("B device_ip=");
    Serial.print(WiFi.localIP());
    Serial.print(" broker=");
    Serial.print(MQTT_HOST);
    Serial.print(':');
    Serial.print(MQTT_PORT);
    Serial.print(" client_id=");
    Serial.print(MQTT_CLIENT_ID);
    Serial.print(" payload=");
    Serial.println(payload);
  } else {
    Serial.println("[mqtt] publish FAILED");
  }
  return ok;
}

}  // namespace

void begin() {
  if (s_state != State::IDLE) return;
  s_client.setServer(MQTT_HOST, MQTT_PORT);
  s_client.setBufferSize(kPubSubBufferSize);  // sized for the §5.1 scene payload
  s_client.setCallback(on_mqtt_message);
  s_state = State::WAIT_WIFI;
  Serial.print("[mqtt] configured broker=");
  Serial.print(MQTT_HOST);
  Serial.print(':');
  Serial.print(MQTT_PORT);
  Serial.print(" client_id=");
  Serial.println(MQTT_CLIENT_ID);
}

void poll(uint32_t now_ms) {
  switch (s_state) {
    case State::IDLE:
      return;

    case State::WAIT_WIFI:
      if (wifi_link::connected()) {
        s_state = State::CONNECTING;
        s_next_attempt_ms = now_ms;  // attempt immediately
      }
      return;

    case State::CONNECTING: {
      if (static_cast<int32_t>(now_ms - s_next_attempt_ms) < 0) {
        return;  // waiting on backoff
      }
      const bool has_auth = (MQTT_USERNAME[0] != '\0');
      const bool ok = has_auth
          ? s_client.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)
          : s_client.connect(MQTT_CLIENT_ID);
      if (ok) {
        s_state = State::CONNECTED;
        s_backoff_ms = kBackoffStartMs;  // FR-5.2 reset on success
        s_last_status_ms = now_ms - kStatusIntervalMs;  // publish immediately
        // FR-1.1 / FR-7.4: re-subscribe on every (re)connect — broker
        // doesn't remember non-persistent sessions across our outages.
        // All three subscriptions go through the same on_mqtt_message
        // dispatcher, which routes by topic.
        const char* const topics[] = { kTopicScene, kTopicClear, kTopicNight, kTopicThermal, kTopicTime };
        for (const char* t : topics) {
          if (s_client.subscribe(t)) {
            Serial.print("[mqtt] sub ");
            Serial.println(t);
          } else {
            Serial.print("[mqtt] sub FAILED ");
            Serial.println(t);
          }
        }
        log_session_info("connected");
      } else {
        schedule_retry(now_ms, "connect failed");
      }
      return;
    }

    case State::CONNECTED:
      if (!wifi_link::connected() || !s_client.connected()) {
        s_backoff_ms = kBackoffStartMs;  // fresh outage
        schedule_retry(now_ms, "session lost");
        return;
      }
      s_client.loop();  // services keepalive + inbound (5.3 will care)
      if (now_ms - s_last_status_ms >= kStatusIntervalMs) {
        s_last_status_ms = now_ms;
        publish_status(now_ms);
      }
      return;

    case State::DISCONNECTED:
      if (!wifi_link::connected()) {
        // Wait for the lower layer to come back before re-trying TCP.
        s_state = State::WAIT_WIFI;
        return;
      }
      if (static_cast<int32_t>(now_ms - s_next_attempt_ms) >= 0) {
        s_state = State::CONNECTING;
      }
      return;
  }
}

bool connected() { return s_state == State::CONNECTED; }

}  // namespace mqtt_link

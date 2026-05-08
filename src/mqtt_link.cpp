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
#include "iss_state.h"
#include "jupiter_state.h"
#include "constellation_state.h"
#include "moon_state.h"
#include "thermal_monitor.h"
#include "time_of_day.h"
#include "wifi_link.h"

// Core 1's render FPS, published once per second from loop1() in
// main.cpp. Declared at file scope (NOT inside the anonymous namespace
// below) so the symbol matches main.cpp's definition at link time.
extern volatile uint32_t g_render_fps;

// Core 1's idle-slack telemetry (FR-16.9, phase D.5). Rolling 32-frame
// average of `kFrameIntervalMs - render_time` in milliseconds. Same
// atomic-uint32 contract as g_render_fps — published once per frame on
// Core 1, read here without a mutex.
extern volatile uint32_t g_render_slack_ms;

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
constexpr const char* kTopicMoon    = "observatory/moon";    // phase 7.2 follow-up
constexpr const char* kTopicIss     = "observatory/iss";     // phase 7.1+ iss data path
constexpr const char* kTopicJupiter = "observatory/jupiter"; // phase 7.3 jupiter data path
constexpr const char* kTopicConstellation = "observatory/constellation"; // phase 7.4 constellation selector
constexpr const char* kTopicDebug   = "observatory/debug";   // phase IR.2 one-shot diagnostic dump (Pico → HA)

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

// IR-learning capture dump (phase IR.2): up to ~9 buttons × ~55 B
// each + envelope ≈ 550 B. 768 leaves ~25 % headroom for future
// fields without crowding NFR-2.1.
constexpr size_t kDebugPayloadCapacity = 768;

// PubSubClient inbound/outbound share a single buffer. Must be ≥ the
// largest payload + topic + a few bytes of MQTT framing. Sized to the
// largest publish/subscribe payload across the whole topic surface.
constexpr size_t kPubSubBufferSize =
    (kSceneJsonCapacity > kDebugPayloadCapacity ? kSceneJsonCapacity
                                                : kDebugPayloadCapacity) + 64;

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
uint32_t s_moon_msgs         = 0;
uint32_t s_moon_rejects      = 0;
uint32_t s_iss_msgs          = 0;
uint32_t s_iss_rejects       = 0;
uint32_t s_jupiter_msgs      = 0;
uint32_t s_jupiter_rejects   = 0;
uint32_t s_constellation_msgs    = 0;
uint32_t s_constellation_rejects = 0;
uint32_t s_clear_msgs        = 0;

// Cross-core one-shot debug publish buffer (phase IR.2). Single
// producer (Core 1, e.g. IrTestScene), single consumer (Core 0's
// poll() drain). Sentinel-0 atomic flag — non-zero = ready, 0 =
// empty — matches CODING_PRACTICES §3 "One-shot diagnostics from
// Core 1 use volatile uint32_t + sentinel 0". Buffer is filled
// BEFORE the flag is set (writer side); reader treats a non-zero
// flag as a happens-before for the buffer contents. Naturally-
// aligned uint32_t writes are atomic on RP2040.
char              s_debug_buf[kDebugPayloadCapacity];
volatile uint32_t s_debug_pending = 0;
uint32_t          s_debug_publishes = 0;
uint32_t          s_debug_drops     = 0;

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

// observatory/moon handler — phase 7.2 follow-up. Payload:
//   {"phase":0.34,"illum_pct":68,"age_d":10,"name":"WAX GIB"}
// `phase` is the synodic-month fraction (0..1, 0 = new, 0.5 = full).
// `illum_pct` and `age_d` are derived but pushed by HA so its UI and
// our panel agree to the integer. `name` is optional (the scene
// derives one if missing). Same FR-1.3 / FR-1.4 discipline as the
// other inbound handlers: malformed → log + drop, never crash.
void handle_moon(char* buf, unsigned int length, uint32_t now_ms) {
  ++s_moon_msgs;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<192> doc;
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, length);
  if (err) {
    ++s_moon_rejects;
    Serial.print("[mqtt] moon parse FAILED err=");
    Serial.print(err.c_str());
    Serial.print(" payload=");
    Serial.println(buf);
    return;
  }

  if (!doc["phase"].is<float>() && !doc["phase"].is<int>()) {
    ++s_moon_rejects;
    Serial.print("[mqtt] moon missing phase payload=");
    Serial.println(buf);
    return;
  }
  const float phase     = doc["phase"].as<float>();
  const int   illum_pct = doc["illum_pct"] | -1;
  const int   age_d     = doc["age_d"]     | -1;
  const char* name      = doc["name"]      | static_cast<const char*>(nullptr);

  if (phase < 0.0f || phase >= 1.0f
   || illum_pct < 0 || illum_pct > 100
   || age_d     < 0 || age_d     > 30) {
    ++s_moon_rejects;
    Serial.print("[mqtt] moon out-of-range phase=");
    Serial.print(phase);
    Serial.print(" illum=");
    Serial.print(illum_pct);
    Serial.print(" age=");
    Serial.println(age_d);
    return;
  }

  moon_state::set_from_mqtt(phase,
                            static_cast<uint8_t>(illum_pct),
                            static_cast<uint16_t>(age_d),
                            name, now_ms);
  Serial.print("[mqtt] moon applied phase=");
  Serial.print(phase);
  Serial.print(" illum=");
  Serial.print(illum_pct);
  Serial.print(" age=");
  Serial.print(age_d);
  Serial.print(" name=");
  Serial.println(name ? name : "(derived)");
}

// observatory/iss handler — phase 7.1++ raw HA pass-through.
//
// HA does NO logic — it just polls public APIs and re-emits the
// fields verbatim through a Jinja template. Wire payload:
//   { "lat_deg": 50.11, "lon_deg": 118.07,
//     "altitude_km": 408, "sunlit": true,
//     "seconds_until_next": 12345,
//     "crew_count": 7 }
//
// • lat_deg/lon_deg/altitude_km/sunlit come from
//   wheretheiss.at /v1/satellites/25544 (.latitude, .longitude,
//   .altitude rounded, .visibility=="daylight").
// • seconds_until_next comes from open-notify iss-pass.json
//   (next response[0].risetime − now()).
// • crew_count is optional (open-notify astros.json filtered to
//   craft=="ISS"); HA may not have polled it yet on cold boot.
//
// All observer-relative geometry (is the station above MY horizon,
// which way to look, am I in darkness) is computed on-device every
// frame in the iss_pass scene from this snapshot + config.h
// LATITUDE_DEG/LONGITUDE_DEG + sun::compute(). Per FR-1.3 / FR-1.4
// any malformed/out-of-range payload is logged and dropped.
void handle_iss(char* buf, unsigned int length, uint32_t now_ms) {
  ++s_iss_msgs;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<256> doc;
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, length);
  if (err) {
    ++s_iss_rejects;
    Serial.print("[mqtt] iss parse FAILED err=");
    Serial.print(err.c_str());
    Serial.print(" payload=");
    Serial.println(buf);
    return;
  }

  // Required fields. Without lat/lon/alt we can't compute look
  // angles; without sunlit we can't decide visibility; without
  // seconds_until_next the countdown is meaningless. Drop the
  // payload entirely if any are missing — the scene falls back to
  // the previous fresh snapshot or "WAIT".
  if (!doc["lat_deg"].is<float>() ||
      !doc["lon_deg"].is<float>() ||
      !doc["altitude_km"].is<int>() ||
      !doc["sunlit"].is<bool>() ||
      !doc["seconds_until_next"].is<long>()) {
    ++s_iss_rejects;
    Serial.print("[mqtt] iss missing required fields payload=");
    Serial.println(buf);
    return;
  }
  const float lat_deg            = doc["lat_deg"].as<float>();
  const float lon_deg            = doc["lon_deg"].as<float>();
  const long  altitude_km_in     = doc["altitude_km"].as<long>();
  const bool  sunlit             = doc["sunlit"].as<bool>();
  const long  seconds_until_next = doc["seconds_until_next"].as<long>();

  // Range checks. ISS altitude lives near 400 km; cap at 999 to fit
  // our uint16 + 3-glyph render slot. seconds_until_next caps at one
  // week — pass predictions further out are almost certainly an HA
  // template bug.
  if (lat_deg < -90.0f || lat_deg > 90.0f ||
      lon_deg < -180.0f || lon_deg > 180.0f) {
    ++s_iss_rejects;
    Serial.print("[mqtt] iss lat/lon out-of-range lat=");
    Serial.print(lat_deg);
    Serial.print(" lon=");
    Serial.println(lon_deg);
    return;
  }
  if (altitude_km_in < 0 || altitude_km_in > 999) {
    ++s_iss_rejects;
    Serial.print("[mqtt] iss altitude_km out-of-range=");
    Serial.println(altitude_km_in);
    return;
  }
  constexpr long kMaxSecondsUntil = 7L * 24L * 60L * 60L;  // 604800
  if (seconds_until_next < 0 || seconds_until_next > kMaxSecondsUntil) {
    ++s_iss_rejects;
    Serial.print("[mqtt] iss out-of-range seconds_until_next=");
    Serial.println(seconds_until_next);
    return;
  }

  // Optional crew_count. Out-of-range demotes to "absent" (renders
  // "?") without rejecting the rest of the payload — FR-1.3 spirit:
  // drop the bad bit, keep the good bits.
  bool    have_crew  = false;
  uint8_t crew_count = 0;
  if (doc["crew_count"].is<int>()) {
    const long crew = doc["crew_count"].as<long>();
    if (crew >= 0 && crew <= 99) {
      have_crew  = true;
      crew_count = static_cast<uint8_t>(crew);
    } else {
      Serial.print("[mqtt] iss crew_count out-of-range=");
      Serial.println(crew);
    }
  }

  iss_state::set_from_mqtt(lat_deg, lon_deg,
                           static_cast<uint16_t>(altitude_km_in),
                           sunlit,
                           static_cast<uint32_t>(seconds_until_next),
                           have_crew, crew_count,
                           now_ms);
  Serial.print("[mqtt] iss applied lat=");
  Serial.print(lat_deg);
  Serial.print(" lon=");
  Serial.print(lon_deg);
  Serial.print(" alt_km=");
  Serial.print(altitude_km_in);
  Serial.print(" sunlit=");
  Serial.print(sunlit ? 1 : 0);
  Serial.print(" seconds_until_next=");
  Serial.print(seconds_until_next);
  if (have_crew) {
    Serial.print(" crew=");
    Serial.print(crew_count);
  }
  Serial.println();
}

// observatory/jupiter handler — phase 7.3 raw HA pass-through.
//
// Same Director/Cinematographer split as the ISS handler: HA polls
// any astronomy integration (e.g. ephemeris/astroweather built on
// pyephem/skyfield) for Jupiter's `azimuth` + `altitude`, and
// re-emits them verbatim via a Jinja template. Wire payload:
//   { "bearing_deg": 90, "elevation_deg": 45,
//     "magnitude": -2.1, "distance_au": 5.4 }
//
// • bearing_deg / elevation_deg are required — without them we
//   can't render the look-here string or decide BELOW/DAY/VIS.
// • magnitude / distance_au are optional ornaments; absent fields
//   render as "?" without rejecting the rest of the payload.
//
// Jupiter is always sunlit (planets shine by reflected light), so
// there is no `sunlit` field — only the observer-side darkness
// condition matters. The jupiter_visibility scene ANDs (elevation
// ≥ 0) with (sun ≤ -6°) every frame on-device.
//
// Per FR-1.3 / FR-1.4 any malformed/out-of-range payload is logged
// and dropped; the previous fresh snapshot keeps rendering.
void handle_jupiter(char* buf, unsigned int length, uint32_t now_ms) {
  ++s_jupiter_msgs;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<256> doc;
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, length);
  if (err) {
    ++s_jupiter_rejects;
    Serial.print("[mqtt] jupiter parse FAILED err=");
    Serial.print(err.c_str());
    Serial.print(" payload=");
    Serial.println(buf);
    return;
  }

  // Required fields. ArduinoJson `is<T>` accepts ints for float
  // slots silently, so we test for numeric presence broadly.
  if (!(doc["bearing_deg"].is<float>()   || doc["bearing_deg"].is<int>()) ||
      !(doc["elevation_deg"].is<float>() || doc["elevation_deg"].is<int>())) {
    ++s_jupiter_rejects;
    Serial.print("[mqtt] jupiter missing required fields payload=");
    Serial.println(buf);
    return;
  }
  const float bearing_in   = doc["bearing_deg"].as<float>();
  const float elevation_in = doc["elevation_deg"].as<float>();

  // Range checks. bearing 0..359 (we wrap 360 → 0), elevation
  // -90..+90.
  if (bearing_in < -1.0f || bearing_in > 360.5f) {
    ++s_jupiter_rejects;
    Serial.print("[mqtt] jupiter bearing_deg out-of-range=");
    Serial.println(bearing_in);
    return;
  }
  if (elevation_in < -90.5f || elevation_in > 90.5f) {
    ++s_jupiter_rejects;
    Serial.print("[mqtt] jupiter elevation_deg out-of-range=");
    Serial.println(elevation_in);
    return;
  }
  // Round + wrap bearing into [0,359]; clamp elevation into [-90,90].
  int b = static_cast<int>(bearing_in + 0.5f);
  if (b >= 360) b -= 360;
  if (b <    0) b += 360;
  int e = static_cast<int>(elevation_in >= 0.0f
                            ? elevation_in + 0.5f
                            : elevation_in - 0.5f);
  if (e >  90) e =  90;
  if (e < -90) e = -90;

  // Optional magnitude. Stored as ×10 fixed-point so the render loop
  // stays float-free (NFR-1.3). Out-of-range demotes to "absent"
  // without rejecting the rest.
  bool    have_magnitude  = false;
  int16_t magnitude_x10   = 0;
  if (doc["magnitude"].is<float>() || doc["magnitude"].is<int>()) {
    const float m = doc["magnitude"].as<float>();
    if (m >= -30.0f && m <= 30.0f) {
      have_magnitude = true;
      magnitude_x10  = static_cast<int16_t>(m >= 0.0f
                                               ? m * 10.0f + 0.5f
                                               : m * 10.0f - 0.5f);
    } else {
      Serial.print("[mqtt] jupiter magnitude out-of-range=");
      Serial.println(m);
    }
  }

  // Optional distance_au, ×10 fixed-point. Jupiter sits at ~4..6 AU
  // in practice; cap at 100 AU for sanity (would catch a sign-flip
  // or a wrong-target template bug).
  bool     have_distance   = false;
  uint16_t distance_au_x10 = 0;
  if (doc["distance_au"].is<float>() || doc["distance_au"].is<int>()) {
    const float d = doc["distance_au"].as<float>();
    if (d >= 0.0f && d <= 100.0f) {
      have_distance   = true;
      distance_au_x10 = static_cast<uint16_t>(d * 10.0f + 0.5f);
    } else {
      Serial.print("[mqtt] jupiter distance_au out-of-range=");
      Serial.println(d);
    }
  }

  // Required constellation_index — same encoding as the
  // observatory/constellation topic (index into the 88-entry IAU
  // catalog in include/stars.h). Drives the scene's daylight
  // readout `IN <IAU>` (e.g. `IN TAU`). Missing or out-of-range
  // rejects the whole payload per FR-1.3 / FR-1.4.
  if (!doc["constellation_index"].is<int>()) {
    ++s_jupiter_rejects;
    Serial.print("[mqtt] jupiter missing constellation_index payload=");
    Serial.println(buf);
    return;
  }
  const long ci_in = doc["constellation_index"].as<long>();
  if (ci_in < 0 || ci_in > 87) {
    ++s_jupiter_rejects;
    Serial.print("[mqtt] jupiter constellation_index out-of-range=");
    Serial.println(ci_in);
    return;
  }
  const uint8_t constellation_index = static_cast<uint8_t>(ci_in);

  jupiter_state::set_from_mqtt(static_cast<int16_t>(b),
                               static_cast<int8_t>(e),
                               have_magnitude, magnitude_x10,
                               have_distance,  distance_au_x10,
                               constellation_index,
                               now_ms);
  Serial.print("[mqtt] jupiter applied bearing=");
  Serial.print(b);
  Serial.print(" elev=");
  Serial.print(e);
  if (have_magnitude) {
    Serial.print(" mag=");
    Serial.print(magnitude_x10 / 10.0f);
  }
  if (have_distance) {
    Serial.print(" dist_au=");
    Serial.print(distance_au_x10 / 10.0f);
  }
  Serial.print(" con_idx=");
  Serial.print(constellation_index);
  Serial.println();
}

// observatory/constellation handler — phase 7.4 selector for the
// constellation_now scene.
//
// Wire payload:
//   { "index": 0, "highlight_star": 1 }
//
// • index           — required, 0..kCatalogCount-1 (88 IAU
//                     constellations). Clamped on the reader side
//                     anyway, but obviously-wrong values are
//                     rejected here so the log shows the bug.
// • highlight_star  — optional, 0..63. -1 (or omit) clears any
//                     highlight. The index is the BRIGHTNESS RANK
//                     among stars rendered for the chosen
//                     constellation: 0 = brightest, 1 = second,
//                     etc. Out-of-range for the chosen entry is
//                     harmless (the scene checks).
//
// Per FR-1.3 / FR-1.4 any malformed payload is logged and dropped;
// the scene keeps using the previous fresh selector or rotates
// locally if none.
void handle_constellation(char* buf, unsigned int length, uint32_t now_ms) {
  ++s_constellation_msgs;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<128> doc;
#pragma GCC diagnostic pop
  const DeserializationError err = deserializeJson(doc, buf, length);
  if (err) {
    ++s_constellation_rejects;
    Serial.print("[mqtt] constellation parse FAILED err=");
    Serial.print(err.c_str());
    Serial.print(" payload=");
    Serial.println(buf);
    return;
  }

  if (!doc["index"].is<int>()) {
    ++s_constellation_rejects;
    Serial.print("[mqtt] constellation missing index payload=");
    Serial.println(buf);
    return;
  }
  const long index_in = doc["index"].as<long>();
  // Sanity-cap at 255 (uint8 max). The catalog will rarely exceed
  // ~50 entries; anything beyond that is a Director bug. The scene's
  // by_index() rejects out-of-range too — defence in depth.
  if (index_in < 0 || index_in > 255) {
    ++s_constellation_rejects;
    Serial.print("[mqtt] constellation index out-of-range=");
    Serial.println(index_in);
    return;
  }

  // Optional highlight. -1 / absent / out-of-range = no highlight.
  bool    have_highlight = false;
  uint8_t highlight_star = 0;
  if (doc["highlight_star"].is<int>()) {
    const long h = doc["highlight_star"].as<long>();
    if (h >= 0 && h <= 63) {
      have_highlight = true;
      highlight_star = static_cast<uint8_t>(h);
    } else if (h != -1) {
      Serial.print("[mqtt] constellation highlight_star out-of-range=");
      Serial.println(h);
    }
  }

  constellation_state::set_from_mqtt(static_cast<uint8_t>(index_in),
                                     have_highlight, highlight_star,
                                     now_ms);
  Serial.print("[mqtt] constellation applied index=");
  Serial.print(index_in);
  if (have_highlight) {
    Serial.print(" highlight_star=");
    Serial.print(highlight_star);
  }
  Serial.println();
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
  if (strcmp(topic, kTopicMoon) == 0) {
    handle_moon(buf, length, millis());
    return;
  }
  if (strcmp(topic, kTopicIss) == 0) {
    handle_iss(buf, length, millis());
    return;
  }
  if (strcmp(topic, kTopicJupiter) == 0) {
    handle_jupiter(buf, length, millis());
    return;
  }
  if (strcmp(topic, kTopicConstellation) == 0) {
    handle_constellation(buf, length, millis());
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
  // Active scene id is owned by Core 1 (mark_current after a swap).
  // The reverse-lookup table lives in scene_state alongside the
  // forward MQTT decoder so the registry stays in one place.
  doc["scene_id"]  = scene_state::string_from_id(scene_state::current());
  // Render FPS is published by Core 1 every second into a single
  // volatile uint32_t (atomic on RP2040, no mutex needed). Declared
  // at file scope above; defined in main.cpp.
  doc["fps"]       = static_cast<uint32_t>(g_render_fps);
  doc["rssi"]      = WiFi.RSSI();
  doc["uptime_s"]  = static_cast<uint32_t>(now_ms / 1000u);
  doc["free_heap"] = static_cast<uint32_t>(rp2040.getFreeHeap());
  // Compositor idle-slack budget (FR-16.9 / phase D.5). Lets HA gauge
  // how much per-frame headroom remains for adding new layers / heavier
  // scenes without violating FR-3.1's 24 FPS target.
  doc["render_slack_ms"] = static_cast<uint32_t>(g_render_slack_ms);

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
        //
        // QoS 1 (at-least-once) on every subscribe: HA publishes at
        // qos:1 (see homeassistant/packages/quantum_observatory.yaml
        // and pyscript/observatory_publisher.py), and effective QoS
        // is min(publisher, subscriber). Subscribing at qos:0 would
        // demote the broker → firmware leg back to fire-and-forget,
        // which is exactly the failure mode we saw in the field
        // (Jupiter screen showing stale az/el because the 15-min
        // pyscript publish was dropped on a Wi-Fi blip). PubSubClient
        // does not support qos:2; qos:1 is the strongest option here
        // and the right one — duplicates are harmless because every
        // payload handler is idempotent (replace-state semantics).
        const char* const topics[] = { kTopicScene, kTopicClear, kTopicNight, kTopicThermal, kTopicTime, kTopicMoon, kTopicIss, kTopicJupiter, kTopicConstellation };
        for (const char* t : topics) {
          if (s_client.subscribe(t, 1)) {
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
      // Drain any pending cross-core debug payload (IrTestScene etc.).
      // Sentinel-0 atomic; only Core 0 ever clears the flag, so the
      // load-then-publish-then-store sequence is race-free against
      // the Core 1 producer (which only WRITES, never reads).
      if (s_debug_pending != 0) {
        const bool ok = s_client.publish(kTopicDebug, s_debug_buf);
        if (ok) {
          ++s_debug_publishes;
          Serial.print("[mqtt] pub ");
          Serial.print(kTopicDebug);
          Serial.print(' ');
          Serial.print(strlen(s_debug_buf));
          Serial.print("B payload=");
          Serial.println(s_debug_buf);
        } else {
          ++s_debug_drops;
          Serial.print("[mqtt] pub ");
          Serial.print(kTopicDebug);
          Serial.println(" FAILED");
        }
        s_debug_pending = 0;  // clear regardless — no retry path
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

void queue_debug(const char* payload) {
  if (payload == nullptr) return;
  // Bounded copy + manual NUL — strncpy would memset the whole tail
  // to zero on every call, which is wasted work for a 768 B buffer
  // hit from a render frame. Truncation is silent (caller is expected
  // to size their payload to the documented capacity); the publish-
  // side log will still show the truncated string.
  size_t n = 0;
  while (n + 1 < sizeof(s_debug_buf) && payload[n] != '\0') {
    s_debug_buf[n] = payload[n];
    ++n;
  }
  s_debug_buf[n] = '\0';
  // Publish AFTER the buffer is fully written. Single-writer,
  // naturally-aligned 32-bit store is atomic on RP2040 — see
  // CODING_PRACTICES §3 sentinel-0 pattern. Use millis() so
  // consecutive bursts (which shouldn't happen but might during
  // bring-up) get distinct values for log readability; OR with 1 so
  // the sentinel is non-zero even when millis() happens to be 0.
  s_debug_pending = millis() | 1u;
}

}  // namespace mqtt_link

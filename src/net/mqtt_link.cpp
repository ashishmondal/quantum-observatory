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
#include "config.h"
#include "ha_discovery.h"
#include "light_sensor.h"
#include "iss_state.h"
#include "planet_state.h"
#include "exoplanet_state.h"
#include "launch_state.h"
#include "constellation_state.h"
#include "moon_state.h"
#include "prefs.h"
#include "theme.h"
#include "thermal_monitor.h"
#include "time_of_day.h"
#include "wifi_link.h"
#ifdef CLOCK_ANIM_TEST
#include "clock_anim_test.h"
#endif

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
constexpr const char* kTopicPlanet = "observatory/planet"; // generic per-body data path (replaces observatory/jupiter)
constexpr const char* kTopicExoplanet = "observatory/exoplanet"; // phase 7.7 NASA Exoplanet Archive stats
constexpr const char* kTopicLaunch  = "observatory/launch";  // phase L     next-launch T-minus data path
constexpr const char* kTopicConstellation = "observatory/constellation"; // phase 7.4 constellation selector
constexpr const char* kTopicTheme   = "observatory/theme";   // FR-15.2 (phase T.4)
constexpr const char* kTopicPrefsReset = "observatory/prefs/reset"; // FR-18.8 (phase P.5)
constexpr const char* kTopicAvailability = "observatory/availability"; // FR-20.3 LWT (phase HA.1)
constexpr const char* kTopicDebug   = "observatory/debug";   // phase IR.2 one-shot diagnostic dump (Pico → HA)
#ifdef CLOCK_ANIM_TEST
// Dev-only: synthetic digit-cascade trigger for the giant clock
// scene. Payload: {"kind":"minute"|"ten_min"|"hour"}. Only
// subscribed under -DCLOCK_ANIM_TEST so the production path stays
// lean. See include/diag/clock_anim_test.h.
constexpr const char* kTopicClockAnimTest = "observatory/test/clock_anim";
#endif

// ── Inbound payload caps ──────────────────────────────────────────
// PRE-REFACTOR FAILURE MODE THAT MOTIVATED THIS BLOCK: the dispatcher
// used to hold ONE shared buffer sized to kSceneJsonCapacity = 384 B
// — but observatory/launch is ~570 B (the ~240-char description
// field dominates). PubSubClient delivered the payload cleanly; the
// dispatcher's oversize gate truncated it before handle_launch() ever
// ran; no counter was bumped; the panel sat on a stale snapshot for
// hours with no signal that anything was wrong. (See git log for the
// fix commit + the post-mortem entry in docs/CODING_PRACTICES.md
// "MQTT topic onboarding".)
//
// The fix is in three parts:
//  1. Per-topic max_payload is declared in the Route table (see
//     kRoutes[] below), colocated with the handler that owns the
//     wire contract — NOT in a top-level constants block where it
//     can drift from the handler.
//  2. The dispatcher buffer is sized to max(kRoutes[*].max_payload)
//     via a constexpr fold (see kInboundJsonCapacity below). Adding
//     a topic with a bigger payload automatically grows the buffer
//     at compile time.
//  3. Oversize-at-dispatch increments the route's own reject counter
//     (was previously a silent drop with only a Serial.print).
//
// The PubSubClient socket buffer must also be ≥ any payload we
// expect to receive. kPubSubBufferSize folds the dispatcher cap +
// the outbound debug-publish cap + framing slack.
//
// §5.4 status example is ~85 B originally; phase HA.3 added sensor /
// diagnostic fields that take the worst-case payload to ~360 B. The
// route-table additions in this commit (`<topic>_msgs/_rejects` for
// every topic) added another ~200 B. NFR-2.3 → max + headroom, kept
// at 768 to leave room for future heartbeat growth.
constexpr size_t kStatusJsonCapacity = 768;

// IR-learning capture dump (phase IR.2): up to ~9 buttons × ~55 B
// each + envelope ≈ 550 B. 768 leaves ~25 % headroom.
constexpr size_t kDebugPayloadCapacity = 768;

WiFiClient   s_tcp;
PubSubClient s_client(s_tcp);

State    s_state            = State::IDLE;
uint32_t s_next_attempt_ms  = 0;
uint32_t s_backoff_ms       = kBackoffStartMs;
// PubSubClient rc captured at the moment of the most recent failed
// connect / dropped session. See mqtt_link.h::last_rc() for the value
// table. Sentinel 0 ("connected") = no outage observed yet — fine on
// a fresh boot, becomes meaningful as soon as the first
// schedule_retry() runs. Single-writer (poll() on Core 0), volatile
// because Core 0's 1 Hz snapshot publisher in main.cpp reads it for
// the InfoOverlayLayer.
volatile int8_t s_last_rc   = 0;
uint32_t s_last_status_ms   = 0;
uint32_t s_status_publishes = 0;
// Per-route inbound counters. The Route table (see kRoutes[] below)
// holds pointers to these — the dispatcher bumps `*rx` once per
// delivered payload (incl. oversize), and the handler bumps `*rej`
// on parse / bounds / cap rejection. Both are emitted on the
// observatory/status heartbeat as `<short>_msgs` / `<short>_rejects`
// so a "broker delivered but device dropped" failure is one heartbeat
// away from being obvious (this is the bug that bit us on phase L —
// the dispatch buffer was 384 B and observatory/launch is ~570 B,
// silently truncated with no counter ever moving).
uint32_t s_scene_msgs           = 0; uint32_t s_scene_rejects         = 0;
uint32_t s_clear_msgs           = 0; uint32_t s_clear_rejects         = 0;
uint32_t s_night_msgs           = 0; uint32_t s_night_rejects         = 0;
uint32_t s_thermal_msgs         = 0; uint32_t s_thermal_rejects       = 0;
uint32_t s_time_msgs            = 0; uint32_t s_time_rejects          = 0;
uint32_t s_moon_msgs            = 0; uint32_t s_moon_rejects          = 0;
uint32_t s_iss_msgs             = 0; uint32_t s_iss_rejects           = 0;
uint32_t s_planet_msgs          = 0; uint32_t s_planet_rejects        = 0;
uint32_t s_exoplanet_msgs       = 0; uint32_t s_exoplanet_rejects     = 0;
uint32_t s_launch_msgs          = 0; uint32_t s_launch_rejects        = 0;
uint32_t s_constellation_msgs   = 0; uint32_t s_constellation_rejects = 0;
uint32_t s_theme_msgs           = 0; uint32_t s_theme_rejects         = 0;
uint32_t s_prefs_reset_msgs     = 0; uint32_t s_prefs_reset_rejects   = 0;
#ifdef CLOCK_ANIM_TEST
uint32_t s_clock_anim_msgs      = 0; uint32_t s_clock_anim_rejects    = 0;
#endif

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

// ── Inbound JSON parse helper ──────────────────────────────────────
// Every observatory/* handler does the same boilerplate: declare a
// fixed-capacity StaticJsonDocument under the v7-deprecation pragma
// guard, run deserializeJson(), on error bump the per-topic reject
// counter and log "[mqtt] <tag> parse FAILED err=… payload=…", then
// drop. Bundling that into a small RAII helper turns ~12 lines of
// boilerplate into two — and confines the deprecation pragma to one
// place (TODO: migrate to JsonDocument when we move past PSC v6).
//
// Usage at every call site:
//   ParsedJson<256> p(buf, length, "iss", s_iss_rejects);
//   if (!p.ok()) return;
//   auto& doc = p.doc();
//   ... // doc[…] as before
template <size_t N>
class ParsedJson {
public:
  ParsedJson(char* buf, unsigned int length,
             const char* tag, uint32_t& reject_counter) {
    err_ = deserializeJson(doc_, buf, length);
    if (err_) {
      ++reject_counter;
      Serial.print("[mqtt] ");
      Serial.print(tag);
      Serial.print(" parse FAILED err=");
      Serial.print(err_.c_str());
      Serial.print(" payload=");
      Serial.println(buf);
    }
  }
  bool ok() const { return !err_; }
  StaticJsonDocument<N>& doc() { return doc_; }

private:
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  StaticJsonDocument<N> doc_;
#pragma GCC diagnostic pop
  DeserializationError err_;
};

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
// `reject_counter` is bound by the per-topic route wrappers below
// so each topic accrues its own reject tally on the wire heartbeat
// (separate `night_rejects` / `thermal_rejects` rather than a
// combined `threshold_rejects` blob).
void handle_thresholds(ThresholdKind kind, char* buf, unsigned int length,
                       uint32_t& reject_counter) {
  const char* tag = (kind == ThresholdKind::NIGHT) ? "night" : "thermal";

  ParsedJson<128> p(buf, length, tag, reject_counter);
  if (!p.ok()) return;
  auto& doc = p.doc();

  // Both fields required — without them there's nothing to apply.
  // Use `.is<int>()` rather than the `| default` shorthand so we can
  // distinguish "missing" from "zero" (zero is a legal, if useless,
  // hysteresis).
  if (!doc["threshold"].is<int>() || !doc["hysteresis"].is<int>()) {
    ++reject_counter;
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
      ++reject_counter;
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
      ++reject_counter;
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

  ParsedJson<128> p(buf, length, "time", s_time_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

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
  ParsedJson<192> p(buf, length, "moon", s_moon_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

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
  ParsedJson<256> p(buf, length, "iss", s_iss_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

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

// observatory/planet handler — generic per-body live-overlay data
// path. Replaces the body-specific observatory/jupiter handler when
// the jupiter scene was generalised into the `planets` scene.
//
// Same Director/Cinematographer split: HA polls an astronomy
// integration (pyephem / skyfield) for the chosen body's look-
// angles and re-emits the result verbatim. Wire payload (see
// docs/MQTT_TOPICS.md):
//   { "name": "jupiter", "bearing_deg": 90, "elevation_deg": 45,
//     "constellation_index": 35 }
//
// • `name` is required and must match one of the
//   planet_renderer / planet_catalog keys (case-insensitive
//   ASCII, ≤ kNameCap-1 chars). The scene only paints the live
//   overlay when the active catalog row's render_name matches.
// • `bearing_deg` (0..359) and `elevation_deg` (-90..+90) are
//   required — without them we can’t paint VIS/BELOW.
// • `constellation_index` (0..87) is required — drives the
//   daylight `IN <IAU>` fallback. Same encoding as the
//   observatory/constellation topic.
//
// All bodies in our catalog are sunlit by reflected light (or are
// the Sun itself), so there is no `sunlit` field — only the
// observer-side darkness condition matters and that's evaluated
// on-device every frame.
//
// Per FR-1.3 / FR-1.4 any malformed / out-of-range payload is
// logged and dropped; the previous fresh snapshot keeps rendering.
void handle_planet(char* buf, unsigned int length, uint32_t now_ms) {
  ParsedJson<256> p(buf, length, "planet", s_planet_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

  // Required `name` — length-bounded so we can't smuggle a
  // pathological string into the cross-core snapshot.
  const char* name_in = doc["name"] | static_cast<const char*>(nullptr);
  if (name_in == nullptr || name_in[0] == '\0') {
    ++s_planet_rejects;
    Serial.print("[mqtt] planet missing name payload=");
    Serial.println(buf);
    return;
  }
  const size_t name_len = strlen(name_in);
  if (name_len >= planet_state::kNameCap) {
    ++s_planet_rejects;
    Serial.print("[mqtt] planet name too long len=");
    Serial.println(name_len);
    return;
  }
  // ASCII-printable gate — every render_name in the catalog is
  // plain a..z, and the case-insensitive match in the scene
  // assumes ASCII. Reject control bytes / multibyte sequences
  // rather than letting them rot in the snapshot.
  for (size_t i = 0; i < name_len; ++i) {
    const unsigned char c = static_cast<unsigned char>(name_in[i]);
    if (c < 32 || c > 126) {
      ++s_planet_rejects;
      Serial.print("[mqtt] planet name non-printable byte at i=");
      Serial.println(i);
      return;
    }
  }

  // Required numeric fields. ArduinoJson's is<float> accepts ints
  // silently for float slots, so test for numeric presence broadly.
  if (!(doc["bearing_deg"].is<float>()   || doc["bearing_deg"].is<int>()) ||
      !(doc["elevation_deg"].is<float>() || doc["elevation_deg"].is<int>())) {
    ++s_planet_rejects;
    Serial.print("[mqtt] planet missing required fields payload=");
    Serial.println(buf);
    return;
  }
  const float bearing_in   = doc["bearing_deg"].as<float>();
  const float elevation_in = doc["elevation_deg"].as<float>();

  if (bearing_in < -1.0f || bearing_in > 360.5f) {
    ++s_planet_rejects;
    Serial.print("[mqtt] planet bearing_deg out-of-range=");
    Serial.println(bearing_in);
    return;
  }
  if (elevation_in < -90.5f || elevation_in > 90.5f) {
    ++s_planet_rejects;
    Serial.print("[mqtt] planet elevation_deg out-of-range=");
    Serial.println(elevation_in);
    return;
  }
  int b = static_cast<int>(bearing_in + 0.5f);
  if (b >= 360) b -= 360;
  if (b <    0) b += 360;
  int e = static_cast<int>(elevation_in >= 0.0f
                            ? elevation_in + 0.5f
                            : elevation_in - 0.5f);
  if (e >  90) e =  90;
  if (e < -90) e = -90;

  // Required constellation_index — same encoding as the
  // observatory/constellation topic (0..87 IAU catalog index).
  if (!doc["constellation_index"].is<int>()) {
    ++s_planet_rejects;
    Serial.print("[mqtt] planet missing constellation_index payload=");
    Serial.println(buf);
    return;
  }
  const long ci_in = doc["constellation_index"].as<long>();
  if (ci_in < 0 || ci_in > 87) {
    ++s_planet_rejects;
    Serial.print("[mqtt] planet constellation_index out-of-range=");
    Serial.println(ci_in);
    return;
  }
  const uint8_t constellation_index = static_cast<uint8_t>(ci_in);

  planet_state::set_from_mqtt(name_in,
                              static_cast<int16_t>(b),
                              static_cast<int8_t>(e),
                              constellation_index,
                              now_ms);
  Serial.print("[mqtt] planet applied name=");
  Serial.print(name_in);
  Serial.print(" bearing=");
  Serial.print(b);
  Serial.print(" elev=");
  Serial.print(e);
  Serial.print(" con_idx=");
  Serial.print(constellation_index);
  Serial.println();
}

// observatory/exoplanet handler — phase 7.7 NASA Exoplanet Archive
// stats data path (FR-14 spirit).
//
// Wire payload (see docs/MQTT_TOPICS.md):
//   {
//     "total_count":         5847,                 // required, 0..2_000_000
//     "added_recent":        12,                   // optional, -10000..+10000
//     "nearest_name":        "Proxima b",          // required, 1..23 ASCII chars
//     "nearest_distance_ly": 4.24                  // optional, 0..6553.5
//   }
//
// Validation (FR-1.3 / FR-1.4 — any failure drops the whole payload,
// the previous fresh snapshot keeps rendering):
//   • total_count is required (uint, sanity-capped at 2_000_000).
//   • nearest_name is required, must fit in 23 chars, ASCII printable
//     (32..126). The procedural-planet renderer seeds from this
//     string; non-printable bytes would still hash but would render
//     as garbage on screen, so we reject early.
//   • added_recent / nearest_distance_ly are optional; out-of-range
//     demotes them to "absent" without rejecting the rest.
void handle_exoplanet(char* buf, unsigned int length, uint32_t now_ms) {
  ParsedJson<256> p(buf, length, "exoplanet", s_exoplanet_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

  // ── Required: total_count ──────────────────────────────────────
  if (!doc["total_count"].is<long>() && !doc["total_count"].is<int>()) {
    ++s_exoplanet_rejects;
    Serial.print("[mqtt] exoplanet missing total_count payload=");
    Serial.println(buf);
    return;
  }
  const long tc_in = doc["total_count"].as<long>();
  if (tc_in < 0 || tc_in > 2000000) {
    ++s_exoplanet_rejects;
    Serial.print("[mqtt] exoplanet total_count out-of-range=");
    Serial.println(tc_in);
    return;
  }
  const uint32_t total_count = static_cast<uint32_t>(tc_in);

  // ── Required: nearest_name ─────────────────────────────────────
  const char* name_in = doc["nearest_name"].as<const char*>();
  if (name_in == nullptr || name_in[0] == '\0') {
    ++s_exoplanet_rejects;
    Serial.print("[mqtt] exoplanet missing nearest_name payload=");
    Serial.println(buf);
    return;
  }
  const size_t name_len = strlen(name_in);
  if (name_len >= exoplanet_state::kNameCap) {
    ++s_exoplanet_rejects;
    Serial.print("[mqtt] exoplanet nearest_name too long len=");
    Serial.println(name_len);
    return;
  }
  for (size_t i = 0; i < name_len; ++i) {
    const unsigned char c = static_cast<unsigned char>(name_in[i]);
    if (c < 32 || c > 126) {
      ++s_exoplanet_rejects;
      Serial.print("[mqtt] exoplanet nearest_name non-ASCII at i=");
      Serial.println(i);
      return;
    }
  }

  // ── Optional: added_recent ─────────────────────────────────────
  bool    have_added_recent = false;
  int16_t added_recent      = 0;
  if (doc["added_recent"].is<int>() || doc["added_recent"].is<long>()) {
    const long a = doc["added_recent"].as<long>();
    if (a >= -10000 && a <= 10000) {
      have_added_recent = true;
      added_recent      = static_cast<int16_t>(a);
    } else {
      Serial.print("[mqtt] exoplanet added_recent out-of-range=");
      Serial.println(a);
    }
  }

  // ── Optional: nearest_distance_ly (×10 fixed-point) ────────────
  bool     have_nearest_distance   = false;
  uint16_t nearest_distance_ly_x10 = 0;
  if (doc["nearest_distance_ly"].is<float>() ||
      doc["nearest_distance_ly"].is<int>()) {
    const float d = doc["nearest_distance_ly"].as<float>();
    if (d >= 0.0f && d <= 6553.5f) {
      have_nearest_distance   = true;
      nearest_distance_ly_x10 = static_cast<uint16_t>(d * 10.0f + 0.5f);
    } else {
      Serial.print("[mqtt] exoplanet nearest_distance_ly out-of-range=");
      Serial.println(d);
    }
  }

  exoplanet_state::set_from_mqtt(total_count,
                                 have_added_recent, added_recent,
                                 name_in,
                                 have_nearest_distance,
                                 nearest_distance_ly_x10,
                                 now_ms);
  Serial.print("[mqtt] exoplanet applied total=");
  Serial.print(total_count);
  Serial.print(" near=");
  Serial.print(name_in);
  if (have_added_recent) {
    Serial.print(" +rec=");
    Serial.print(added_recent);
  }
  if (have_nearest_distance) {
    Serial.print(" dist_ly=");
    Serial.print(nearest_distance_ly_x10 / 10.0f);
  }
  Serial.println();
}

// observatory/launch handler — phase L next-scheduled-rocket-launch
// T-minus data path (FR-14.6).
//
// Wire payload (see docs/MQTT_TOPICS.md and FR-14.6):
//   {
//     "t0_local_epoch": 1739481600,            // required, HA-local epoch
//     "t0_estimate": false,                    // required bool
//     "t0_window_close_local_epoch": 0,        // optional, 0 = none
//     "provider": "SX",                        // required ≤ kProviderCap-1
//     "vehicle":  "FALCON 9",                  // required ≤ kVehicleCap-1
//     "mission":  "STARLINK 17-42",            // required ≤ kMissionCap-1
//     "pad_code": "VSF",                       // required ≤ kPadCodeCap-1
//     "result":   -1                           // optional, -1..2 (default -1)
//   }
//
// Time frame: all epoch fields are HA-local wall seconds since 1970
// (NOT UTC). HA's pyscript publisher does the UTC→local conversion
// once at the producer so the firmware needs zero tz state for the
// countdown — it's a pure subtraction against tod::now().local_epoch.
//
// Range / consistency rules (FR-1.3 / FR-1.4 — drop the whole
// payload on any failure, previous fresh snapshot keeps rendering):
//   • t0_local_epoch     in [now_local-3600, now_local+8640000]
//                         (1 h slip .. 100 d ahead). If the RTC is
//                         not yet trusted, we skip the time-of-day
//                         bound (only structural validation runs).
//   • t0_window_close_local_epoch
//                         in [t0, t0+86400] when present (windows
//                         beyond 24 h are almost certainly a
//                         producer bug).
//   • provider / vehicle / mission / pad_code must be non-empty
//     and fit the snapshot field with room for a NUL.
//   • result              must be in [-1, 2] when present.
void handle_launch(char* buf, unsigned int length, uint32_t now_ms) {
  // Buffer sized to comfortably fit the maximum legal payload:
  // the small numeric fields + four short strings + the ~240-char
  // description (kDescriptionCap-1). ArduinoJson v7 needs ~1.5x the
  // serialized size for the DOM; 1024 gives ~640 bytes payload
  // headroom, well above the worst-case ~480 bytes.
  ParsedJson<1024> p(buf, length, "launch", s_launch_rejects);
  if (!p.ok()) {
    return;
  }
  auto& doc = p.doc();

  // --- t0_local_epoch (required) ---
  if (!doc["t0_local_epoch"].is<long>() && !doc["t0_local_epoch"].is<int>()) {
    ++s_launch_rejects;
    Serial.print("[mqtt] launch missing t0_local_epoch payload=");
    Serial.println(buf);
    return;
  }
  const long t0_in = doc["t0_local_epoch"].as<long>();

  // Bounds check against the live local-epoch when the RTC is
  // trusted; otherwise we let the scene's freshness gate clip
  // anything that ages out. Same frame as the wire — no tz math.
  const tod::Reading r = tod::now(now_ms);
  if (r.valid) {
    const long now_local = static_cast<long>(r.local_epoch);
    if (t0_in < now_local - 3600L || t0_in > now_local + 8640000L) {
      ++s_launch_rejects;
      Serial.print("[mqtt] launch t0_local_epoch out-of-range=");
      Serial.print(t0_in);
      Serial.print(" now_local=");
      Serial.println(now_local);
      return;
    }
  }
  const int32_t t0_local_epoch = static_cast<int32_t>(t0_in);

  // --- t0_estimate (required bool) ---
  if (!doc["t0_estimate"].is<bool>()) {
    ++s_launch_rejects;
    Serial.print("[mqtt] launch missing t0_estimate payload=");
    Serial.println(buf);
    return;
  }
  const bool t0_estimate = doc["t0_estimate"].as<bool>();

  // --- t0_window_close_local_epoch (optional, default 0) ---
  int32_t t0_window_close_local_epoch = 0;
  if (doc["t0_window_close_local_epoch"].is<long>() ||
      doc["t0_window_close_local_epoch"].is<int>()) {
    const long w = doc["t0_window_close_local_epoch"].as<long>();
    if (w != 0) {
      if (w < t0_in || w > t0_in + 86400L) {
        ++s_launch_rejects;
        Serial.print("[mqtt] launch t0_window_close_local_epoch out-of-range=");
        Serial.print(w);
        Serial.print(" t0=");
        Serial.println(t0_in);
        return;
      }
      t0_window_close_local_epoch = static_cast<int32_t>(w);
    }
  }

  // --- result (optional, default -1) ---
  int8_t result = -1;
  if (doc["result"].is<int>()) {
    const long rs = doc["result"].as<long>();
    if (rs < -1 || rs > 2) {
      ++s_launch_rejects;
      Serial.print("[mqtt] launch result out-of-range=");
      Serial.println(rs);
      return;
    }
    result = static_cast<int8_t>(rs);
  }

  // --- strings (required) ---
  // Use the `|` default-fallback operator that the scene_id /
  // theme handlers also use — ArduinoJson v7's `is<const char*>`
  // probe is finicky against strings backed by the document's
  // internal pool (it returns false for strings that DID parse
  // successfully but happen to have been deduped/owned), so the
  // safer idiom is to pipe through nullptr and check the result.
  auto fetch_string = [&](const char* key, uint8_t cap,
                          const char** out_ptr) -> bool {
    const char* s = doc[key] | static_cast<const char*>(nullptr);
    if (s == nullptr || s[0] == '\0') {
      ++s_launch_rejects;
      Serial.print("[mqtt] launch missing ");
      Serial.print(key);
      Serial.print(" payload=");
      Serial.println(buf);
      return false;
    }
    if (strlen(s) > static_cast<size_t>(cap - 1)) {
      ++s_launch_rejects;
      Serial.print("[mqtt] launch ");
      Serial.print(key);
      Serial.print(" too long (>");
      Serial.print(cap - 1);
      Serial.print(") value=");
      Serial.println(s);
      return false;
    }
    *out_ptr = s;
    return true;
  };
  const char* provider = nullptr;
  const char* vehicle  = nullptr;
  const char* mission  = nullptr;
  const char* pad_code = nullptr;
  if (!fetch_string("provider", launch_state::kProviderCap, &provider)) return;
  if (!fetch_string("vehicle",  launch_state::kVehicleCap,  &vehicle))  return;
  if (!fetch_string("mission",  launch_state::kMissionCap,  &mission))  return;
  if (!fetch_string("pad_code", launch_state::kPadCodeCap,  &pad_code)) return;

  // --- description (optional) ---
  // Cosmetic mission blurb scrolled in the bottom marquee. Optional
  // and length-checked like the other strings, but absence is fine
  // (the scene falls back to the provider/vehicle/mission tag line).
  // HA's pyscript is expected to ASCII-fold + clip to kDescriptionCap-1
  // before publishing, so anything longer means upstream contract
  // breach and we reject the whole payload to surface it.
  const char* description = doc["description"]
      | static_cast<const char*>(nullptr);
  if (description != nullptr) {
    if (strlen(description) > launch_state::kDescriptionCap - 1u) {
      ++s_launch_rejects;
      Serial.print("[mqtt] launch description too long (>");
      Serial.print(launch_state::kDescriptionCap - 1);
      Serial.print(") len=");
      Serial.println(strlen(description));
      return;
    }
  } else {
    description = "";  // copy_clamped() treats this as "clear field"
  }

  // --- org / pad_country (optional friendlier-name fields) ---
  // Used by the launch_countdown scene's typewriter info row to
  // surface the full provider name ("SPACEX") and a country-level
  // location ("FL, USA" / "NEW ZEALAND") in place of the compact
  // `provider` / `pad_code` tags. Both are optional — absence is
  // fine; the scene falls back gracefully on empty strings.
  auto fetch_optional = [&](const char* key, uint8_t cap,
                            const char** out_ptr) -> bool {
    const char* s = doc[key] | static_cast<const char*>(nullptr);
    if (s == nullptr) {
      *out_ptr = "";
      return true;
    }
    if (strlen(s) > static_cast<size_t>(cap - 1)) {
      ++s_launch_rejects;
      Serial.print("[mqtt] launch ");
      Serial.print(key);
      Serial.print(" too long (>");
      Serial.print(cap - 1);
      Serial.print(") len=");
      Serial.println(strlen(s));
      return false;
    }
    *out_ptr = s;
    return true;
  };
  const char* org         = "";
  const char* pad_country = "";
  if (!fetch_optional("org",         launch_state::kOrgCap,        &org))         return;
  if (!fetch_optional("pad_country", launch_state::kPadCountryCap, &pad_country)) return;

  launch_state::set_from_mqtt(t0_local_epoch, t0_estimate,
                              t0_window_close_local_epoch,
                              result, provider, vehicle, mission, pad_code,
                              org, pad_country, description, now_ms);
  Serial.print("[mqtt] launch applied t0_local=");
  Serial.print(t0_in);
  Serial.print(t0_estimate ? " (NET)" : " (CONF)");
  if (t0_window_close_local_epoch != 0) {
    Serial.print(" win_close=");
    Serial.print(t0_window_close_local_epoch);
  }
  Serial.print(" ");
  Serial.print(provider);
  Serial.print(" ");
  Serial.print(vehicle);
  Serial.print(" \"");
  Serial.print(mission);
  Serial.print("\" pad=");
  Serial.print(pad_code);
  Serial.print(" result=");
  Serial.println(result);
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
  ParsedJson<128> p(buf, length, "constellation", s_constellation_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

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

// observatory/theme handler — FR-15.2 active-theme selector + FR-15.6
// image-tint strength. Payload:
//   {"id": "<theme_id>"}            // theme only
//   {"tint": 0..100}                // strength only
//   {"id": "<theme_id>", "tint": N} // both, applied atomically
// At least one of `id` / `tint` MUST be present. The wire id is a
// lowercase enumerator name (e.g. "apollo_amber"); the canonical
// mapping lives in theme::id_from_string. Unknown ids and out-of-range
// tints are logged and the WHOLE payload is dropped per FR-1.3 — we
// never half-apply state. prefs::set_theme() / set_image_tint_pct()
// are themselves idempotent (no-op when value is unchanged), so HA
// echoing the current values on reconnect is harmless and does NOT
// mark the prefs cache dirty. Per FR-15.4 the swap is next-frame,
// no scene re-init. Per FR-18.3 / FR-15.2 each change is persisted
// via the wear-protected writeback so it survives a power cycle.
void handle_theme(char* buf, unsigned int length, uint32_t /*now_ms*/) {
  ParsedJson<96> p(buf, length, "theme", s_theme_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

  const bool has_id   = doc.containsKey("id");
  const bool has_tint = doc.containsKey("tint");
  if (!has_id && !has_tint) {
    ++s_theme_rejects;
    Serial.print("[mqtt] theme missing id/tint payload=");
    Serial.println(buf);
    return;
  }

  // Validate everything BEFORE applying anything (FR-1.3 — never
  // half-apply: a payload with a good id and a bad tint must be
  // rejected as a whole, not partially applied).
  theme::Id id = theme::current();
  if (has_id) {
    const char* wire_id = doc["id"] | static_cast<const char*>(nullptr);
    if (wire_id == nullptr || wire_id[0] == '\0' ||
        !theme::id_from_string(wire_id, &id)) {
      ++s_theme_rejects;
      Serial.print("[mqtt] theme bad/unknown id payload=");
      Serial.println(buf);
      return;
    }
  }
  uint8_t tint = theme::image_tint_pct();
  if (has_tint) {
    // Use a sentinel default outside the valid range so a non-int /
    // missing field surfaces here even though we already gated on
    // containsKey() above (defensive against ArduinoJson coercion
    // surprises on string / float inputs).
    const int v = doc["tint"] | -1;
    if (v < 0 || v > 100) {
      ++s_theme_rejects;
      Serial.print("[mqtt] theme tint out of range payload=");
      Serial.println(buf);
      return;
    }
    tint = static_cast<uint8_t>(v);
  }

  // Apply tint first so the rebuild triggered by set_theme() (when
  // the id also changed) already uses the new strength. Both setters
  // are idempotent on no-op so applying both is cheap.
  if (has_tint) prefs::set_image_tint_pct(tint);
  if (has_id)   prefs::set_theme(id);
  Serial.print("[mqtt] theme applied");
  if (has_id)   { Serial.print(" id=");   Serial.print(theme::string_from_id(id)); }
  if (has_tint) { Serial.print(" tint="); Serial.print(static_cast<int>(tint)); }
  Serial.println();
}

#ifdef CLOCK_ANIM_TEST
// Dev-only: synthetic digit-cascade trigger for the giant clock
// scene. Writes g_clock_anim_test_kind; the scene edge-detects on
// the next frame and resets the byte. Out-of-range / unknown kinds
// are dropped silently — same discipline as the production
// handlers, no crash on garbage payload.
void handle_clock_anim_test(char* buf, unsigned int length, uint32_t /*now_ms*/) {
  ParsedJson<96> p(buf, length, "clock_anim_test", s_theme_rejects);
  if (!p.ok()) return;
  auto& doc = p.doc();

  const char* kind = doc["kind"] | static_cast<const char*>(nullptr);
  if (kind == nullptr || kind[0] == '\0') {
    Serial.print("[mqtt] clock_anim_test missing kind payload=");
    Serial.println(buf);
    return;
  }
  uint8_t v = 0;
  if      (strcmp(kind, "minute")  == 0) v = static_cast<uint8_t>(clock_anim_test::Kind::MINUTE);
  else if (strcmp(kind, "ten_min") == 0) v = static_cast<uint8_t>(clock_anim_test::Kind::TEN_MIN);
  else if (strcmp(kind, "hour")    == 0) v = static_cast<uint8_t>(clock_anim_test::Kind::HOUR);
  else {
    Serial.print("[mqtt] clock_anim_test unknown kind=");
    Serial.println(kind);
    return;
  }
  g_clock_anim_test_kind = v;
  Serial.print("[mqtt] clock_anim_test fired kind=");
  Serial.println(kind);
}
#endif

// ── Extracted handlers (uniform signature for the Route table) ────
//
// `scene`, `clear`, `prefs/reset`, and the night/thermal threshold
// wrappers used to live inline at the bottom of on_mqtt_message().
// Pulling them out so every topic dispatches through one mechanism
// — the kRoutes[] table below — keeps the dispatcher tiny and
// guarantees the per-topic counters / capacity caps stay in lock-step
// with the handler they describe. Same return contract as the rest:
// log + drop on any validation failure, never crash, never propagate
// to the renderer.

void handle_night(char* buf, unsigned int length, uint32_t /*now_ms*/) {
  handle_thresholds(ThresholdKind::NIGHT, buf, length, s_night_rejects);
}

void handle_thermal(char* buf, unsigned int length, uint32_t /*now_ms*/) {
  handle_thresholds(ThresholdKind::THERMAL, buf, length, s_thermal_rejects);
}

void handle_clear(char* /*buf*/, unsigned int /*length*/, uint32_t /*now_ms*/) {
  // §5.3: payload is empty by spec. Don't validate it — a non-empty
  // payload is harmless noise and rejecting it would just give the
  // Director a footgun. clear_sticky() is itself a no-op when no
  // sticky scene is active (FR-2.2).
  scene_state::clear_sticky();
  Serial.println("[mqtt] clear_sticky");
}

void handle_prefs_reset(char* /*buf*/, unsigned int /*length*/, uint32_t /*now_ms*/) {
  // FR-18.8 — destructive escape hatch. Payload is empty by spec
  // (mirrors clear_sticky); we accept any payload as the trigger
  // because the topic itself is the gate — anyone publishing here
  // is intentionally asking for a wipe. prefs::reset() deletes
  // /prefs.json and disarms the writeback pipeline, then we hand
  // off to rp2040.reboot() so FR-18.5 boot-restore re-applies
  // stock defaults. Logged before the reboot so the operator can
  // tell from serial that the trigger landed.
  Serial.println("[mqtt] prefs/reset received — wiping and rebooting");
  prefs::reset();
  Serial.flush();
  rp2040.reboot();
  // unreachable
}

void handle_scene(char* buf, unsigned int length, uint32_t /*now_ms*/) {
  // ParsedJson<384> bundles the v7 deprecation pragma (StaticJsonDocument
  // is exactly the static-buffer behaviour we want — JsonDocument's
  // default allocator uses malloc/free which violates NFR-2.2 on this
  // hot path) plus the parse-or-reject boilerplate. The 384 DOM size
  // mirrors the route's max_payload cap (see kRoutes[]).
  ParsedJson<384> p(buf, length, "scene", s_scene_rejects);
  if (!p.ok()) return;  // FR-1.4: malformed JSON does not interrupt active scene
  auto& doc = p.doc();

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
  // active scene keeps rendering.
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

  const bool accepted = scene_state::request(id, prio_u8, dur_u16, sticky,
                                             /*user_intent=*/true);  // explicit operator command always wins
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

// ── Route table — the single source of truth for inbound topics ───
//
// Each row binds one MQTT topic to its handler + its per-topic
// payload cap + its rx/reject counters. Adding a topic = adding a
// row; the dispatcher, the subscribe loop, the status heartbeat,
// and the dispatch-buffer sizing all read from this same table.
//
// max_payload notes (NFR-2.3 — max documented + headroom):
//   * scene        — §5.1, ~150 B body, FR-4.4 caps overrides.text at
//                    ~14 chars → 384 with 150 % headroom
//   * launch       — FR-14.6, ~570 B (~240-char description dominates)
//                    → 768 (THE bug from phase L: was being truncated
//                    by a 384 B shared buffer, silent drop, no counter)
//   * moon / iss / jupiter / constellation / time / theme / clear /
//     prefs/reset  — small numeric / short-string bodies, < 256 B
//   * night / thermal — 2 ints, < 96 B
//   * clock_anim_test — dev only, < 96 B
struct Route {
  const char* topic;
  size_t      max_payload;  // upper bound on the JSON payload in bytes;
                            // dispatcher rejects oversized payloads
  void (*handler)(char* buf, unsigned int length, uint32_t now_ms);
  uint32_t*   rx_counter;       // bumped by the dispatcher on every delivery
  uint32_t*   reject_counter;   // bumped by handler on validation failure
                                // AND by the dispatcher on oversize
  const char* short_name;       // emitted in heartbeat as `<short>_msgs/_rejects`
};

constexpr Route kRoutes[] = {
  { kTopicScene,         384, handle_scene,         &s_scene_msgs,         &s_scene_rejects,         "scene"        },
  { kTopicClear,          32, handle_clear,         &s_clear_msgs,         &s_clear_rejects,         "clear"        },
  { kTopicNight,          96, handle_night,         &s_night_msgs,         &s_night_rejects,         "night"        },
  { kTopicThermal,        96, handle_thermal,       &s_thermal_msgs,       &s_thermal_rejects,       "thermal"      },
  { kTopicTime,          128, handle_time,          &s_time_msgs,          &s_time_rejects,          "time"         },
  { kTopicMoon,          256, handle_moon,          &s_moon_msgs,          &s_moon_rejects,          "moon"         },
  { kTopicIss,           256, handle_iss,           &s_iss_msgs,           &s_iss_rejects,           "iss"          },
  { kTopicPlanet,        256, handle_planet,        &s_planet_msgs,        &s_planet_rejects,        "planet"       },
  { kTopicExoplanet,     256, handle_exoplanet,     &s_exoplanet_msgs,     &s_exoplanet_rejects,     "exoplanet"    },
  { kTopicLaunch,        768, handle_launch,        &s_launch_msgs,        &s_launch_rejects,        "launch"       },
  { kTopicConstellation, 128, handle_constellation, &s_constellation_msgs, &s_constellation_rejects, "constellation"},
  { kTopicTheme,         128, handle_theme,         &s_theme_msgs,         &s_theme_rejects,         "theme"        },
  { kTopicPrefsReset,     32, handle_prefs_reset,   &s_prefs_reset_msgs,   &s_prefs_reset_rejects,   "prefs_reset"  },
#ifdef CLOCK_ANIM_TEST
  { kTopicClockAnimTest,  96, handle_clock_anim_test, &s_clock_anim_msgs,  &s_clock_anim_rejects,    "clock_anim"   },
#endif
};

// Dispatcher buffer size = the largest payload across the whole
// route table. Constexpr fold so the buffer auto-grows the moment a
// new (or expanded) route declares a bigger max_payload. This is the
// invariant that the pre-refactor code violated (shared buffer was
// hard-coded to scene's cap, not max-over-routes).
constexpr size_t fold_max_payload() {
  size_t m = 0;
  for (const auto& r : kRoutes) if (r.max_payload > m) m = r.max_payload;
  return m;
}
constexpr size_t kInboundJsonCapacity = fold_max_payload();

// PubSubClient inbound/outbound share a single socket buffer; it must
// be ≥ the largest payload we receive OR publish, plus topic name +
// ~5 B MQTT framing. +64 covers the worst-case topic length comfortably.
constexpr size_t kPubSubBufferSize =
    ((kInboundJsonCapacity > kDebugPayloadCapacity)
         ? kInboundJsonCapacity : kDebugPayloadCapacity) + 64;

// Belt-and-braces guard: if a future heartbeat addition pushes
// kStatusJsonCapacity past the socket buffer, the publish would
// fail silently inside PubSubClient. Trip the build instead.
static_assert(kStatusJsonCapacity + 64 <= kPubSubBufferSize,
              "status payload exceeds PubSubClient buffer");

// PubSubClient inbound callback. Runs on Core 0 from inside
// PubSubClient::loop() (called from poll()) — same thread as the rest
// of mqtt_link, so no locking needed against our own static state.
// Validation rules (FR-1.3, FR-1.4): malformed JSON or unknown topic
// is logged and dropped; we never crash and never propagate to the
// renderer.
void on_mqtt_message(char* topic, uint8_t* payload, unsigned int length) {
  // Find the route for this topic. O(n) over a tiny table — same
  // cost as the previous strcmp chain.
  const Route* route = nullptr;
  for (const auto& r : kRoutes) {
    if (strcmp(topic, r.topic) == 0) { route = &r; break; }
  }
  if (route == nullptr) {
    return;  // defensive; we only subscribed to topics in kRoutes
  }

  // Per-route delivery counter — bumped BEFORE the size gate so an
  // operator watching the heartbeat sees the rx tick even when the
  // payload turns out to be unusable. Reject counter then captures
  // the unusable subset.
  ++*route->rx_counter;

  // Per-route oversize gate. Previously this was a single shared cap
  // sized to scene only (384 B), which silently truncated launch
  // (~570 B) — no counter moved, no on-panel signal. Now each topic
  // declares its own cap (see kRoutes[].max_payload) and an oversize
  // counts as a reject so the next observatory/status heartbeat
  // surfaces it.
  if (length > route->max_payload) {
    ++*route->reject_counter;
    Serial.print("[mqtt] oversize topic=");
    Serial.print(topic);
    Serial.print(" len=");
    Serial.print(length);
    Serial.print(" cap=");
    Serial.println(route->max_payload);
    return;
  }

  // Common buffer-copy step: every topic we handle is JSON, every
  // handler wants a NUL-terminated C-string. Shared dispatch buffer
  // is sized to fold_max_payload() over kRoutes so any single payload
  // that fits its own route's cap fits here too.
  char buf[kInboundJsonCapacity];
  memcpy(buf, payload, length);
  buf[length] = '\0';

  // millis() is fine here — the callback runs from PubSubClient::loop()
  // on Core 0, the same thread that owns tod's smoothing baseline.
  route->handler(buf, length, millis());
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
  // Snapshot the PubSubClient rc BEFORE we change state — once we
  // flip s_state to DISCONNECTED, future reads from the layer should
  // see "the reason this attempt failed", not whatever the client's
  // internal field decays to on the next loop().
  s_last_rc = static_cast<int8_t>(s_client.state());
  Serial.print("[mqtt] ");
  Serial.print(reason);
  Serial.print(" rc=");
  Serial.print(static_cast<int>(s_last_rc));
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
  // Active retro sci-fi theme id (FR-15.7 / phase T.4). Lets HA confirm
  // the device's state without round-tripping observatory/theme.
  doc["theme"] = theme::string_from_id(theme::current());
  // FR-15.6 / FR-15.7 — active image-tint strength (0..100). Echoed
  // alongside the theme so HA can drive a single number entity
  // without round-tripping observatory/theme.
  doc["image_tint_pct"] = static_cast<int>(theme::image_tint_pct());
  // FR-18.7 — surfaces whether the in-RAM prefs cache has changes
  // not yet flushed to /prefs.json. HA (and the FR-17.8 info
  // overlay) use this to confirm a setting has been durably saved;
  // expect a brief `true` window of ≤ 35 s after a theme change
  // (5 s settle + worst-case 30 s rate cap; first-ever flush skips
  // the rate cap so it lands at settle+0).
  doc["prefs_dirty"] = prefs::is_dirty();

  // FR-19 / FR-18.2 v3 — sound prefs heartbeat. Pure echo so HA can
  // surface them as toggles / a select without re-publishing on
  // every change (the `set` topic remains the write path).
  {
    const prefs::Prefs& p = prefs::current();
    doc["theme_sound"]     = p.theme_sound;
    doc["button_sound"]    = p.button_sound;
    doc["tick_sound_mode"] = static_cast<int>(p.tick_sound_mode);
  }

  // FR-20 — sensor + link diagnostics consumed by the auto-discovered
  // HA entities (phase HA.3). Re-using observatory/status (vs minting
  // new per-entity state topics) means steady-state MQTT chatter is
  // identical to pre-phase-HA: one ~500 B message every 30 s.

  // Ambient light (FR-7.1). Raw 12-bit ADC: HIGHER = DARKER on this
  // hardware (see config.h LIGHT_NIGHT_THRESHOLD_DEFAULT). The
  // `night` boolean is the debounced Schmitt-decision already used by
  // the safety overlay; exposing both lets HA build a calibration
  // chart AND drive automations.
  doc["light_raw"] = light_sensor::raw();
  doc["night"]     = light_sensor::is_night();

  // DS3231 silicon temperature (FR-7.3). last_temp_c() returns
  // INT8_MIN before the first successful poll; publish JSON null in
  // that window so HA shows "unknown" instead of -128 °C.
  {
    const int8_t t = thermal_monitor::last_temp_c();
    if (t == INT8_MIN) doc["temp_c"] = nullptr;
    else               doc["temp_c"] = static_cast<int>(t);
  }
  doc["hot"] = thermal_monitor::is_hot();

  // MQTT link diagnostics — echoed so the HA device card surfaces
  // the same precise outage detail the info overlay shows on-panel
  // (FR-17.8 / IR.4). mqtt_state is a short enum string so the HA
  // entity reads naturally in automations; mqtt_rc + mqtt_backoff_s
  // round it out for support-style queries. We're inside CONNECTED
  // by construction here (publish_status only fires from that
  // branch), so the value is always "connected" on the wire — but
  // we render it generically anyway so a future "publish a final
  // status on disconnect" path can reuse the same code.
  {
    const char* state_str = "connected";
    switch (s_state) {
      case State::IDLE:         state_str = "idle";         break;
      case State::WAIT_WIFI:    state_str = "wait_wifi";    break;
      case State::CONNECTING:   state_str = "connecting";   break;
      case State::CONNECTED:    state_str = "connected";    break;
      case State::DISCONNECTED: state_str = "disconnected"; break;
    }
    doc["mqtt_state"]     = state_str;
    doc["mqtt_rc"]        = static_cast<int>(s_last_rc);
    doc["mqtt_backoff_s"] = static_cast<uint32_t>(s_backoff_ms / 1000u);
  }

  // Per-topic inbound rx + reject counters — one pair per row in
  // kRoutes. Renders as `scene_msgs` / `scene_rejects` / `launch_msgs`
  // / `launch_rejects` / …. Lets an operator without serial-console
  // access tell `broker delivered, device dropped` (msgs up, rejects
  // up) from `broker never delivered` (both flat) without uploading
  // a debug build — the failure mode that hid the phase-L 384 B
  // dispatch-buffer bug for hours.
  {
    char key[40];  // `<short_name>_rejects` ≤ ~24 B; 40 = comfy
    for (const auto& r : kRoutes) {
      snprintf(key, sizeof(key), "%s_msgs", r.short_name);
      doc[key] = *r.rx_counter;
      snprintf(key, sizeof(key), "%s_rejects", r.short_name);
      doc[key] = *r.reject_counter;
    }
  }

  // Phase L launch context — the live applied snapshot's t0 and the
  // signed t_minus against the RTC. These are NOT counters (those
  // are above); they exist so an operator can verify "the device
  // accepted the new launch and is computing the right T-minus"
  // without paging through serial. `launch_t0=0` means no snapshot
  // is held (or it's aged past kFreshMs). `launch_t_minus_s=null`
  // means we have a snapshot but no RTC reading yet.
  {
    launch_state::Snapshot ls;
    if (launch_state::get(now_ms, &ls) && ls.valid) {
      doc["launch_t0"] = static_cast<int32_t>(ls.t0_local_epoch);
      const tod::Reading r = tod::now(now_ms);
      if (r.valid) {
        doc["launch_t_minus_s"] =
            static_cast<int32_t>(ls.t0_local_epoch -
                                 static_cast<int32_t>(r.local_epoch));
      } else {
        doc["launch_t_minus_s"] = nullptr;
      }
    } else {
      doc["launch_t0"]        = 0;
      doc["launch_t_minus_s"] = nullptr;
    }
  }

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
  // PubSubClient defaults MQTT_SOCKET_TIMEOUT to 15 s, which is the
  // wall-clock budget for the blocking TCP connect() inside
  // s_client.connect(). When the broker is unreachable (e.g. HA host
  // down, but Wi-Fi still up) that call blocks the whole Core 0
  // loop() for the full 15 s — longer than the 8 s NFR-3.2 watchdog
  // (see main.cpp::setup() rp2040.wdt_begin). The WDT then resets
  // the chip before mqtt_link::poll() ever returns, the new boot
  // hits the same code path, and the device boot-loops for as long
  // as the broker is gone (instead of falling through to the
  // OfflineScene override per FR-5.1). 3 s is plenty for a same-LAN
  // broker, fits in the WDT budget with the FR-3.1 frame work, and
  // a failed connect just falls back into the FR-5.2 backoff
  // schedule. PubSubClient::loop()/publish() are non-blocking on a
  // healthy session (they peek at WiFiClient::available() first),
  // so this only narrows the connect path that was actually
  // misbehaving.
  s_client.setSocketTimeout(3);
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
      // FR-20.3 Last-Will-Testament: ask the broker to publish a
      // retained "offline" on observatory/availability the moment our
      // keepalive lapses or the TCP socket drops. PubSubClient's
      // 8-arg connect() signature is (clientId, user, pass, willTopic,
      // willQoS, willRetain, willMessage). We use qos:1 + retain=true
      // so HA's MQTT integration sees the same offline marker on a
      // fresh subscribe long after the device went dark. Matched by
      // the retained "online" publish below on every successful
      // (re)connect — same retain flag so HA always reads ground
      // truth from the retained value, not from a missed live edge.
      const bool has_auth = (MQTT_USERNAME[0] != '\0');
      const bool ok = has_auth
          ? s_client.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD,
                             kTopicAvailability, /*willQoS=*/1,
                             /*willRetain=*/true, /*willMessage=*/"offline")
          : s_client.connect(MQTT_CLIENT_ID,
                             /*willTopic=*/kTopicAvailability,
                             /*willQoS=*/1, /*willRetain=*/true,
                             /*willMessage=*/"offline");
      if (ok) {
        s_state = State::CONNECTED;
        s_backoff_ms = kBackoffStartMs;  // FR-5.2 reset on success
        s_last_status_ms = now_ms - kStatusIntervalMs;  // publish immediately
        // FR-20.3: announce "online" retained immediately so HA
        // entities flip available before the first discovery config
        // or status heartbeat is processed. Idempotent — the broker
        // is happy to overwrite its own retained value.
        s_client.publish(kTopicAvailability, "online", /*retain=*/true);
        Serial.print("[mqtt] pub ");
        Serial.print(kTopicAvailability);
        Serial.println(" online (retain)");
        // FR-20.1: publish the 15 Home Assistant Discovery config
        // messages so the device + every entity registers without
        // any manual HA YAML. One-shot per session; configs are
        // retained at the broker so HA picks them up on the next
        // restart without us re-publishing.
        ha_discovery::publish_all(s_client);
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
        // Subscribe to every topic in the route table. Single source
        // of truth — adding a new route auto-extends the subscribe
        // set without a parallel edit here.
        for (const auto& r : kRoutes) {
          if (s_client.subscribe(r.topic, 1)) {
            Serial.print("[mqtt] sub ");
            Serial.println(r.topic);
          } else {
            Serial.print("[mqtt] sub FAILED ");
            Serial.println(r.topic);
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

State state() { return s_state; }

uint32_t backoff_ms() {
  return s_state == State::CONNECTED ? 0u : s_backoff_ms;
}

int8_t last_rc() { return s_last_rc; }

uint32_t launch_rx_count()     { return s_launch_msgs; }
uint32_t launch_reject_count() { return s_launch_rejects; }

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

void publish_button_event(const char* name) {
  // FR-11.2 echo. Drop silently when the broker session is down so
  // the local action (info overlay) still happens regardless of MQTT
  // state. Synchronous publish is fine here -- we're on Core 0,
  // PubSubClient::publish() is non-blocking on a healthy session
  // (CODING_PRACTICES sec 3), and the payload is tiny.
  if (name == nullptr || !s_client.connected()) return;
  char payload[40];
  const int n = snprintf(payload, sizeof(payload),
                         "{\"button\":\"%s\"}", name);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(payload)) return;
  s_client.publish("observatory/button", payload);
  Serial.print("[mqtt] pub observatory/button ");
  Serial.println(payload);
}

}  // namespace mqtt_link

// MQTT link manager — owns the broker connection on Core 0. Layered
// on top of wifi_link (which must be CONNECTED before we attempt
// any TCP work). Same non-blocking begin()/poll() pattern as wifi_link
// (CODING_PRACTICES §3 "network modules are non-blocking state
// machines").
//
// Subscriptions (re-issued on every reconnect):
//   observatory/scene    — FR-1.1 scene trigger → scene_state::request()
//   observatory/night    — FR-7.4 LDR thresholds → light_sensor::set_thresholds()
//   observatory/thermal  — FR-7.4 DS3231 thresholds → thermal_monitor::set_thresholds()
//   observatory/time     — FR-9.5 RTC correction → tod::set_from_mqtt()
//
// Publishes:
//   observatory/status   — §5.4 heartbeat every 30 s
//   observatory/debug    — phase IR.2 one-shot diagnostic dumps
//                          (cross-core via queue_debug())
//
// All inbound payloads are validated per FR-1.3 / FR-1.4: malformed
// JSON or out-of-range fields are logged and dropped, never
// propagated past the callback. (added in phase 5.2; extended through
// phases 5.3 → 5.6)

#pragma once

#include <stdint.h>

namespace mqtt_link {

enum class State : uint8_t {
  IDLE         = 0,  // before begin()
  WAIT_WIFI    = 1,  // wifi_link not yet CONNECTED
  CONNECTING   = 2,  // PubSubClient.connect() in progress (single shot)
  CONNECTED    = 3,  // broker session established
  DISCONNECTED = 4,  // outage; backoff timer running
};

// One-time setup. Stores broker config from secrets.h, primes the
// state machine. Idempotent.
void begin();

// Drive the state machine. Call from loop() at any rate ≥ 1 Hz; safe
// to call every iteration. Internally also calls PubSubClient::loop()
// to service the keepalive when CONNECTED.
void poll(uint32_t now_ms);

// True iff the broker session is up.
bool connected();

// Cross-core one-shot publish to `observatory/debug`. Safe to call
// from Core 1 (e.g. from a scene's render() path). Copies `payload`
// into a static buffer guarded by a sentinel-0 atomic flag; Core 0's
// poll() drains and publishes on the next iteration when CONNECTED.
// `payload` MUST be a complete JSON document (no envelope is added).
// Truncated to the buffer capacity if oversize. If a previous
// queue_debug() is still pending, this call overwrites it. Intended
// for one-shot diagnostics (e.g. IR-learning capture dump), NOT
// steady-state telemetry — use `observatory/status` for that.
// (added in phase IR.2)
void queue_debug(const char* payload);

}  // namespace mqtt_link

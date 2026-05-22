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
//   observatory/status         — §5.4 heartbeat every 30 s
//   observatory/availability   — FR-20.3 LWT (retained: "online" on
//                                connect, "offline" auto-published
//                                by the broker on outage)
//   homeassistant/<comp>/...   — FR-20.1 MQTT-Discovery configs,
//                                one retained publish per entity on
//                                each successful connect
//   observatory/debug          — phase IR.2 one-shot diagnostic dumps
//                                (cross-core via queue_debug())
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

// Current state machine value. Read by the IR.4 InfoOverlayLayer
// (via the 1 Hz Core 0 → Core 1 snapshot in main.cpp) so an operator
// staring at the panel can tell WAIT_WIFI ("MQTT WAIT") apart from
// CONNECTING / DISCONNECTED ("MQTT RETRY 8s") apart from CONNECTED.
State state();

// Current retry-backoff delay in milliseconds (FR-5.2 schedule).
// Returns 0 when the broker session is up. Surfaced on the panel as
// "RETRY %us" so the operator can sanity-check the schedule from
// across the room without serial console access.
uint32_t backoff_ms();

// PubSubClient::state() captured at the moment of the most recent
// failed connect / dropped session. Values follow the PubSubClient
// rc convention:
//    -4 timeout, -3 lost, -2 socket/connect_failed,  -1 disconnected,
//     0 connected,  1 bad_protocol, 2 bad_client_id, 3 unavailable,
//     4 bad_credentials, 5 unauthorized.
// Returns 0 ("connected", the int8 sentinel for "no error captured
// yet") on a fresh boot before any outage. Used by the IR.4
// InfoOverlayLayer to render the precise failure mode in 7 chars
// (SOCKET / AUTH / PROTO / …) instead of a generic "offline".
int8_t last_rc();

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

// FR-11.2 — echo a logical on-board button press to
// `observatory/button` as `{"button":"<name>"}`. Fire-and-forget on
// Core 0; silently no-ops when not CONNECTED so the local action
// (info overlay) can still fire regardless of broker state. `name`
// MUST be a short ASCII identifier from the FR-11 mapping ("menu",
// "up", "down"); not validated here — callers are the small set of
// onboard_buttons.cpp call sites.
void publish_button_event(const char* name);

// Inbound-counter accessors for the §5.4 status heartbeat. Phase L
// debugging — observatory/launch is the largest payload (~480 B) and
// the easiest one to lose silently (oversize buffer, JSON parse,
// out-of-range bounds). Surfacing the rx + reject counts on the
// heartbeat lets an operator without serial-console access tell
// "publisher fired but device dropped it" (msgs steady, rejects up)
// from "broker never delivered" (both steady) without uploading a
// debug build.
uint32_t launch_rx_count();
uint32_t launch_reject_count();

}  // namespace mqtt_link

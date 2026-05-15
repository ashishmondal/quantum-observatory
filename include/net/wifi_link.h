// Wi-Fi link manager — owns the CYW43439 connection state on the
// Pico W. Lives entirely on Core 0 (NFR-3.1 — render core never
// touches the radio). Non-blocking: begin() kicks off the connect
// asynchronously; poll() drives state transitions.
//
// FR-5.2: reconnects with exponential backoff (1s → 60s, doubling).
// We wire the backoff machinery now even though Phase 6.4 is the
// formal "offline fallback" deliverable — the link layer is the
// natural home for it and 5.2 (MQTT) needs to assume the link
// behaves sanely under outage.
//
// (added in phase 5.1)

#pragma once

#include <stdint.h>

namespace wifi_link {

enum class State : uint8_t {
  IDLE         = 0,  // before begin()
  CONNECTING   = 1,  // SSID join in progress
  CONNECTED    = 2,  // got an IP
  DISCONNECTED = 3,  // lost link, waiting for backoff
};

// Bring up the radio and start the first connect attempt. Reads
// WIFI_SSID / WIFI_PASSWORD from secrets.h. Idempotent — safe to
// call once from setup().
void begin();

// Drive the state machine. Call from loop() at any rate ≥ 1 Hz.
// Cheap when nothing's changing.
void poll(uint32_t now_ms);

// Current state. Diagnostics; MQTT (Phase 5.2) reads this to know
// when it can attempt a broker connection.
State state();

// True iff state() == CONNECTED. Convenience for hot paths.
bool connected();

// Returns the current backoff delay in milliseconds (0 when
// connected). Diagnostics / status heartbeat use this.
uint32_t backoff_ms();

}  // namespace wifi_link

// On-board MENU button driver (FR-11.1, minimal subset).
//
// Phase S.1 wires only the GP15 MENU button — UP / DOWN remain
// reserved per FR-11.3. Polled on Core 0 from loop() with a ≥30 ms
// debounce window per FR-11.1; each accepted falling-edge press
// fires the FR-17.8 info overlay (via the same g_info_overlay_event_ms
// primitive the IR.4 OK action used to drive) and, when MQTT is up,
// echoes to `observatory/button` per FR-11.2.
//
// FR-19 divergence note: the user-facing intent for MENU is "always
// show the info overlay, regardless of MQTT state". That's a
// deliberate override of FR-11.3's "no-op when MQTT is connected"
// gate — the in-room debug UX wins over the strict spec. The MQTT
// echo still publishes so HA can react if it wants to.
//
// All API is Core 0 only — no cross-core safety needed.

#pragma once

#include <stdint.h>

namespace onboard_buttons {

// One-time bring-up. pinMode + initial state read so the first
// tick() can edge-detect cleanly without seeing a phantom press.
void begin();

// Drive the debounce + edge-detect state machine. Cheap when no
// transition is occurring (one digitalRead). Call from Core 0
// loop() at any rate ≥ 100 Hz.
void tick(uint32_t now_ms);

}  // namespace onboard_buttons

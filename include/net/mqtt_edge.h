// FR-5.1 / FR-6.5+ — MQTT link state edge detection.
//
// Drives two override scenes:
//   1. OFFLINE: edge-detected on mqtt_link::connected() flips, calls
//      scene_state::set_offline_active() so the panel switches to
//      OfflineScene within FR-5.1's 5 s budget.
//   2. SPLASH: latched-clear on the first time MQTT comes online,
//      so the boot splash dismisses when the device is fully ready.
//      Subsequent disconnects fall through the normal offline path,
//      never back to splash.
//
// Lives on Core 0 next to mqtt_link::tick(); call once per loop()
// pass — internally rate-limits log output to one line per edge.

#pragma once

#include <stdint.h>

namespace mqtt_edge {

void tick(uint32_t now_ms);

}  // namespace mqtt_edge

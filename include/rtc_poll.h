// 1 Hz RTC poll cadence + reject-backoff. Lives on Core 0 alongside
// every other I²C consumer. See FR-9.5 (RTC = single read path) and
// FR-13.5 (validated read path with consensus + outer cadence).
//
// Default cadence is 1 hour on accept; a rejected read backs off
// exponentially (1 s → 2 s → … → 1 h cap) until it accepts again.

#pragma once

#include <stdint.h>

namespace rtc_poll {

// Drive the schedule. Cheap when nothing is due (one wrap-safe
// compare). Call once per outer 1 Hz log tick on Core 0.
void tick(uint32_t now_ms);

}  // namespace rtc_poll

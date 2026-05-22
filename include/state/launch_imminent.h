// Launch-imminent auto-switch tick (FR-14.6 / FR-2.1).
//
// Rising edge of (fresh launch_state ∧ valid RTC ∧ 0 ≤ t_minus ≤
// kImminentWindowS) preempts the active scene with LAUNCH_COUNTDOWN
// at the maximum priority (5, sticky) and plays a short attention
// melody so the operator turns to the panel before liftoff. Mirrors
// the iss_visibility::tick() pattern.
//
// Falling edge — t_minus drops below the post-t0 hold floor OR the
// snapshot ages out of fresh — clears the sticky only when the
// active scene is still LAUNCH_COUNTDOWN (an operator who navigated
// away during the countdown keeps their choice).

#pragma once

#include <stdint.h>

namespace launch_imminent {

// How many seconds before t0 we promote the countdown scene. 5 min
// matches the user-facing requirement and is the LL2 publish jitter
// budget (poll cadence = 10 min, so a +5 min lead-time gives the
// rising edge at least one fresh snapshot before liftoff).
constexpr int32_t kImminentWindowS = 5 * 60;

// Call from Core 0 loop(). Internal 1 Hz rate-limit.
void tick(uint32_t now_ms);

}  // namespace launch_imminent

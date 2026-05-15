// ISS visibility predicate + auto-switch tick.
//
// "Visible now" means the AND of:
//   (a) fresh iss_state snapshot (HA pushed coords + sunlit flag),
//   (b) ISS is sunlit (NORAD shadow flag false),
//   (c) ISS is above the observer's horizon (look-angle elevation ≥ 0),
//   (d) observer is in civil twilight or darker (sun ≤ -6°).
//
// This single source of truth is consumed by:
//   - IssPassScene::render — picks the "VIS" hue when true,
//   - iss_visibility::tick — rising-edge auto-switches to ISS_PASS at
//     priority 4 sticky and plays the "ta-da" alert melody.

#pragma once

#include <stdint.h>

namespace iss_visibility {

// Self-contained predicate. ~150 µs incl. trig (sun::compute is the
// dominant cost). Safe to call every frame from Core 1; safe to call
// at 1 Hz from Core 0.
bool is_visible_now(uint32_t now_ms);

// Edge-detector + auto-switch + ta-da. Call from Core 0 loop().
// Internal 1 Hz rate-limit keeps the trig cost negligible.
void tick(uint32_t now_ms);

}  // namespace iss_visibility

// Optional Core-0 stress harnesses. Both are compile-time gated and
// compile away to nothing when the macro isn't defined; safe to call
// unconditionally from loop().
//
//   -DCORE0_STRESS      — JSON-parse + checksum CPU burner (NFR-1.1
//                         / NFR-3.1 validation; phase 4.3).
//   -DCORE0_MQTT_FLOOD  — 20 Hz scene_state::request() flood
//                         (FR-16.7 seqlock validation; phase D.4).

#pragma once

#include <stdint.h>

namespace stress_harness {

void tick(uint32_t now_ms);

}  // namespace stress_harness

// FR-17.5 IR remote dispatch — local-fast button actions.
//
// Each accepted IR press routes through scene_state::request() at the
// operator-cycle priority/duration documented in FR-17.6, with
// user_intent=true so they bypass the FR-2.1 priority gate even when
// a sticky firmware-initiated scene (e.g. the priority-4 ISS
// auto-switch) is active.

#pragma once

#include <stdint.h>

class FontDemoScene;

namespace ir_actions {

// Install the FR-17.5 dispatch table on the IR receiver. The
// `font_demo` pointer is captured for the LEFT/RIGHT carveout that
// drives FontDemoScene's font picker when the diagnostic is up; pass
// nullptr if FONT_DEMO is not built.
void install_dispatch_table(FontDemoScene* font_demo);

}  // namespace ir_actions

// Cross-core IR.4 trigger. action_ir_info_toggle() (in ir_actions.cpp)
// writes the press timestamp; Core 1's InfoOverlayLayer edge-detects
// new values. Single naturally-aligned uint32 is atomic on RP2040 —
// no mutex (CODING_PRACTICES §3). Sentinel 0 = "no event".
extern volatile uint32_t g_info_overlay_event_ms;

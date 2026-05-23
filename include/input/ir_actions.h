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

// Core-0 reconciler tick. Call once per loop() iteration from main.cpp.
// Cheap when nothing is pending — one scene_state::current() read +
// one compare. Clears scene-focus state if the active scene changed
// out from under us (MQTT Director, ISS auto-switch, sticky expiry,
// firmware override — anything that bypasses ir_actions). Without
// this, ▲/▼ would still be captured by a "ghost" focus after, say,
// HA swaps the scene from the dashboard.
void tick(uint32_t now_ms);

}  // namespace ir_actions

// Cross-core IR.4 trigger. action_ir_info_toggle() (in ir_actions.cpp)
// writes the press timestamp; Core 1's InfoOverlayLayer edge-detects
// new values. Single naturally-aligned uint32 is atomic on RP2040 —
// no mutex (CODING_PRACTICES §3). Sentinel 0 = "no event".
extern volatile uint32_t g_info_overlay_event_ms;

// Cross-core scene-focus key event. Written by Core 0 (ir_actions.cpp)
// when a key arrives for a focused scene; read by Core 1 (compositor)
// which edge-detects new values and dispatches Scene::on_key() on the
// active scene. Packed: low 8 bits = SceneKey, high 24 bits = monotonic
// counter. Sentinel 0 = "no event ever fired" (matches g_info_overlay_event_ms
// discipline so a wraparound to the counter==0, key==0 state cannot
// be confused with the boot value). Single naturally-aligned uint32 ⇒
// atomic on RP2040, no mutex (CODING_PRACTICES §3). The single-slot
// design accepts that a press arriving inside the ~42 ms render frame
// can overwrite an unread one — IR.4's repeat suppression (FR-17.4)
// already prevents a stampede so this is bounded to "operator hammered
// the remote faster than ~24 Hz".
extern volatile uint32_t g_scene_key_event;

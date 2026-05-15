// FR-16 Compositor — Layer stack walk, safety/info overlays, fade
// transitions, render-loop telemetry.
//
// Owns the active scene pointer (g_current_scene), the LayerSlot
// stack (g_layers), the FadeBlackLayer state machine, and the
// cross-core render telemetry volatiles consumed by Core 0
// (mqtt_link status heartbeat, IR.4 InfoOverlayLayer, watchdog).
//
// Lifecycle (Core 1):
//   setup1():   compositor::init_default_scene(matrix);
//               compositor::install_safety_overlays(matrix);
//   loop1():    compositor::tick(matrix, now_ms);
//
// Threading: tick() runs on Core 1 only. Telemetry globals are
// naturally-aligned uint32_t — atomic on RP2040 (CODING_PRACTICES §3),
// readable from Core 0 without a mutex.

#pragma once

#include <Adafruit_Protomatter.h>
#include <stdint.h>

namespace compositor {

// Run the active scene's init() once at boot.
void init_default_scene(Adafruit_Protomatter& matrix);

// Bind the four safety override scenes (SPLASH / THERMAL / NIGHT /
// OFFLINE) into LAYER_OVERLAY_SAFETY and run their one-time init().
void install_safety_overlays(Adafruit_Protomatter& matrix);

// One render iteration: consumes pending scene swaps, walks the
// layer stack, calls matrix.show(), updates render telemetry.
void tick(Adafruit_Protomatter& matrix, uint32_t now_ms);

}  // namespace compositor

// ─── Cross-core render telemetry (Core 1 → Core 0) ──────────────────
// All four follow the atomic-uint32 contract (CODING_PRACTICES §3) —
// single naturally-aligned 32-bit store/load on RP2040, no mutex.

// Frames rendered in the last completed wall-clock second. Republished
// once per second by compositor::tick(). Read by mqtt_link status
// publish and by InfoOverlayLayer.
extern volatile uint32_t g_render_fps;

// millis() at the start of the most recent loop1 iteration. Watchdog
// uses this to detect a Core 1 stall (no frames for kRenderStallMs ⇒
// don't pet → reboot, NFR-3.2).
extern volatile uint32_t g_render_alive_ms;

// 32-frame rolling average of (kFrameIntervalMs - render_ms). Exposed
// via the §5.4 status heartbeat as `render_slack_ms`. 0 means we're
// frame-cap-bound.
extern volatile uint32_t g_render_slack_ms;

// One-shot first-frame render time after a scene swap. Set to render_ms
// (≥1) by the post-swap branch; cleared to 0 by Core 0 after logging.
extern volatile uint32_t g_first_frame_render_ms;

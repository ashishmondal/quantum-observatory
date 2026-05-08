// Cross-core scene state — Core 0 (Gatekeeper, MQTT) writes intent;
// Core 1 (Artist, render) reads it once per frame and decides whether
// to swap the active Scene. Single mutex_t guards the whole struct
// (CODING_PRACTICES §3 — multi-field cross-core state needs a real
// mutex, not volatile).
//
// Lifecycle of a scene change:
//   Core 0  → scene_state::request(SceneId::CLOCK)
//   Core 1  → if (scene_state::take_pending(&id)) swap_to(id)
//
// take_pending() returns true exactly once per *effective* state
// change. Phase D.3 (FR-16.2) narrowed the meaning of "effective":
// take_pending() now reflects only the Director's intent (the
// `mqtt_requested` field). Firmware-owned overrides (night, thermal,
// offline, splash) are layered as compositor overlays on top of the
// Director scene by `SafetyOverlayLayer`, which reads the flags via
// `read_overrides()`. The underlying scene continues to render and
// animate while an overlay is engaged, so on override clear the user
// sees a fade rather than a scene restart.
//
// Re-requesting the same scene is a no-op (no spurious re-init).
// The "current" id is tracked separately in scene_state::current() so
// status heartbeats (FR-5.4 territory) can report it without forcing
// a re-init.
//
// Adding a new scene id = (a) one enum value, (b) one case in the
// dispatcher (main.cpp swap_to). Nothing in this header changes per
// scene — keeps NFR-5.1 honest. (added in phase 4.2; night override
// added in phase 5.5.1; thermal_safe override added in phase 5.5.2;
// override-as-overlay model adopted in phase D.3)

#pragma once

#include <stdint.h>

namespace scene_state {

// Stable, compact scene identifier. The MQTT scene_id string (FR-1.2)
// will map to this enum in Phase 5.4. Keep ordering append-only —
// values may be logged or persisted.
enum class SceneId : uint8_t {
  BOOT          = 0,
  CLOCK         = 1,   // giant clock — default idle (FR-9.4)
  COLOR_CYCLE   = 2,
  TEXT_DEMO     = 3,
  BG_STARFIELD  = 4,
  BG_PARALLAX   = 5,
  BG_NEBULA     = 6,
  NIGHT         = 7,   // dim room-clock — firmware override (FR-7.2)
  THERMAL_SAFE  = 8,   // dim cool-down — firmware override (FR-7.3)
  GFX_TEST      = 9,   // smoke-test pattern: gradients + palette cycle + FPS
  BG_BITMAP     = 10,  // bitmap-backed background with palette cycling
  BG_IMAGE      = 11,  // direct RGB565 image from assets/*.bmp (no animation)
  OFFLINE       = 12,  // MQTT-disconnect fallback — firmware override (FR-5.1)
  SPLASH        = 13,  // boot splash — firmware override, shown until first MQTT connect
  SKY_TIMELAPSE = 14,  // debug: 1 day every 10 s, sun rises L → sets R
  ISS_PASS      = 15,  // priority-4 "ISS NOW" callout (phase 7.1)
  MOON_PHASE    = 16,  // sticky moon disc + phase readout (phase 7.2)
  JUPITER_VISIBILITY = 17,  // Jupiter look-angles + magnitude readout (phase 7.3)
  CONSTELLATION_NOW  = 18,  // dynamic constellation art + name (phase 7.4)
  IR_TEST            = 19,  // IR receiver POC readout (phase IR.1)
  FONT_DEMO          = 20,  // diagnostic: cycle Adafruit_GFX builtin fonts
};

// One-time mutex init. Call from setup() before either core spins.
void init();

// Writer (Core 0). Records the Director's desired scene plus its
// priority (FR-2.1, 0..5; values are clamped) and lifecycle hints
// (FR-2.3 duration / FR-2.4 hard TTL).
//
// Drops the request if the new priority is strictly less than the
// active scene's priority — see CODING_PRACTICES §3 "priority
// preemption". Returns true when accepted, false when dropped.
//
// duration_s: revert-to-default delay for non-sticky scenes, in
//   seconds (FR-2.3, default 30). Ignored when sticky.
// sticky:     when true, scene only ends via clear_sticky, the FR-2.4
//   hard 1 h TTL, or another sticky preempting it.
//
// All scenes are capped by a hard kHardTtlSec (1 h, FR-2.4) regardless
// of duration / sticky.
//
// Equal-priority requests are accepted (latest-wins, REQUIREMENTS §9
// default). No-op (returns true) when id + priority + sticky already
// match the current Director intent.
bool request(SceneId id, uint8_t priority = 1,
             uint16_t duration_s = 30, bool sticky = false);

// Writer (Core 0). Drives FR-2.3/FR-2.4 expiry. Call once per
// loop() iteration with millis(); cheap when no deadline is pending.
// On expiry the active Director scene is reverted to the default
// (CLOCK at priority 0, per FR-9.4). Firmware overrides are
// untouched — they have their own lifecycles.
void tick(uint32_t now_ms);

// Writer (Core 0). Sets the FR-7.2 firmware override. While true,
// take_pending() resolves to NIGHT regardless of request(). Falling
// edge naturally re-exposes the previously-requested scene.
void set_night_active(bool active);

// Writer (Core 0). Sets the FR-7.3 firmware override. Highest
// priority — preempts both night_active and the Director's request
// (FR-7.5). Falling edge re-exposes whatever the lower-priority
// resolution would otherwise pick. (added in phase 5.5.2)
void set_thermal_active(bool active);

// Writer (Core 0). FR-2.2 / §5.3 `observatory/clear_sticky` handler.
// If the active Director scene is sticky, reverts to the default
// (CLOCK @ priority 0, FR-9.4). No-op when nothing sticky is active —
// non-sticky scenes already auto-expire via tick(). (added in phase 6.3)
void clear_sticky();

// Writer (Core 0). FR-5.1 firmware override — driven by mqtt_link's
// connected() state. While active, take_pending() resolves to OFFLINE
// regardless of the Director's last request, but is preempted by both
// safety overrides (thermal_safe > night > offline > director). The
// Director's last request is preserved across the outage and naturally
// reappears when MQTT reconnects (same pattern as night/thermal).
// (added in phase 6.4)
void set_offline_active(bool active);

// Writer (Core 0). Boot splash override — highest priority of all
// firmware overrides so the splash holds the screen until the device
// has finished its first network handshake. Cleared exactly once,
// when MQTT first transitions to connected; the falling edge re-
// exposes whatever the lower-priority resolution would otherwise
// pick (typically the giant clock).
void set_splash_active(bool active);

// Reader (Core 1). Atomically returns true + writes the *resolved*
// scene id into *out exactly once per effective-state change. On
// false, no scene change is pending — caller continues rendering the
// current scene.
bool take_pending(SceneId* out);

// Tracking accessors (any core). current() is what's actually being
// rendered (Core 1 sets this after a successful swap). pending() is
// what's queued; mostly for diagnostics — production code should use
// take_pending() to consume.
SceneId current();
void    mark_current(SceneId id);  // Core 1 calls after a swap

// Reader (Core 1). Snapshots all four firmware override flags under
// a single mutex acquire, so the SafetyOverlayLayer's per-frame
// "which override wins" decision sees a consistent view (FR-16.2).
// Pass nullptr for any flag you don't care about. (added in phase D.3)
void read_overrides(bool* splash, bool* thermal, bool* night, bool* offline);

// Resolve a wire-format scene_id string (FR-1.2 / §6 registry) to a
// SceneId. Returns true on hit and writes the value to *out; false
// for unknown / empty / null strings (caller should log+drop per
// FR-1.3). The mapping table lives next to the enum so adding a
// scene is one enum value + one row here + one case in the renderer
// dispatcher (NFR-5.1 spirit). (added in phase 5.4)
bool id_from_string(const char* s, SceneId* out);

// Reverse of id_from_string — returns the wire-format string for a
// SceneId, or "unknown" if the id isn't in the registry. Used by the
// status heartbeat (§5.4) to report the active scene without the MQTT
// layer needing to know about the registry table.
const char* string_from_id(SceneId id);

}  // namespace scene_state

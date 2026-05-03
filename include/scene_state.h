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
// change. The effective scene is computed from two sources:
//   1. mqtt_requested — what the Director (HA over MQTT) last asked for
//   2. firmware-owned overrides (FR-7.5): night_active forces NIGHT
//      regardless of mqtt_requested; thermal_active preempts even
//      night with THERMAL_SAFE.
// Resolution rule (firmware-owned safety > Director intent):
//   resolved = thermal_active ? THERMAL_SAFE
//            : night_active   ? NIGHT
//            : mqtt_requested
// Re-requesting the same scene is a no-op (no spurious re-init).
// The "current" id is tracked separately in scene_state::current() so
// status heartbeats (FR-5.4 territory) can report it without forcing
// a re-init.
//
// Adding a new scene id = (a) one enum value, (b) one case in the
// dispatcher (main.cpp swap_to). Nothing in this header changes per
// scene — keeps NFR-5.1 honest. (added in phase 4.2; night override
// added in phase 5.5.1; thermal_safe override added in phase 5.5.2)

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
};

// One-time mutex init. Call from setup() before either core spins.
void init();

// Writer (Core 0). Records the Director's desired scene. No-op if
// already the current Director intent. The effective scene Core 1
// renders may differ — see take_pending() resolution rule.
void request(SceneId id);

// Writer (Core 0). Sets the FR-7.2 firmware override. While true,
// take_pending() resolves to NIGHT regardless of request(). Falling
// edge naturally re-exposes the previously-requested scene.
void set_night_active(bool active);

// Writer (Core 0). Sets the FR-7.3 firmware override. Highest
// priority — preempts both night_active and the Director's request
// (FR-7.5). Falling edge re-exposes whatever the lower-priority
// resolution would otherwise pick. (added in phase 5.5.2)
void set_thermal_active(bool active);

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

// Resolve a wire-format scene_id string (FR-1.2 / §6 registry) to a
// SceneId. Returns true on hit and writes the value to *out; false
// for unknown / empty / null strings (caller should log+drop per
// FR-1.3). The mapping table lives next to the enum so adding a
// scene is one enum value + one row here + one case in the renderer
// dispatcher (NFR-5.1 spirit). (added in phase 5.4)
bool id_from_string(const char* s, SceneId* out);

}  // namespace scene_state

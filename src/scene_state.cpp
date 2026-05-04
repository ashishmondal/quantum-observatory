// Implementation of include/scene_state.h. See header for the IPC model.
//
// All access goes through the mutex; copy out under lock, work outside.
// (CODING_PRACTICES §3)

#include "scene_state.h"

#include <string.h>

#include <Arduino.h>     // millis() — used to stamp expiry deadlines
#include <pico/mutex.h>

namespace scene_state {

namespace {

// FR-2.4: every scene, sticky or not, hard-caps at 1 h to prevent a
// permanent lock by a misbehaving Director.
constexpr uint32_t kHardTtlMs = 60u * 60u * 1000u;

// FR-2.3: non-sticky default if the Director omits `duration`.
constexpr uint16_t kDefaultDurationSec = 30;

struct State {
  SceneId  mqtt_requested  = SceneId::CLOCK;  // Director intent (FR-1)
  uint8_t  mqtt_priority   = 0;               // FR-2.1; boot default is the floor
  bool     sticky          = false;           // FR-2.2
  uint32_t expires_at_ms   = 0;               // 0 = no soft deadline (sticky); soft = duration end
  uint32_t hard_ttl_at_ms  = 0;               // FR-2.4 absolute cap
  SceneId  current         = SceneId::BOOT;   // last marked by renderer
  bool     night_active    = false;           // FR-7.2 firmware override
  bool     thermal_active  = false;           // FR-7.3 firmware override (highest priority)
  bool     offline_active  = false;           // FR-5.1 firmware override (mqtt-disconnect)
  bool     splash_active   = false;           // boot splash override (highest)
  bool     dirty           = true;            // resolved-state changed since last take_pending
};

State   s_state;
mutex_t s_mutex;

// Resolution rule — firmware-owned safety/ambient overrides preempt
// Director intent. Mirror this in scene_state.h's docblock when the
// priority list changes. (FR-7.5: thermal > night > director;
// phase 6.4: offline slots between night and director.)
SceneId resolve(const State& s) {
  if (s.splash_active)  return SceneId::SPLASH;
  if (s.thermal_active) return SceneId::THERMAL_SAFE;
  if (s.night_active)   return SceneId::NIGHT;
  if (s.offline_active) return SceneId::OFFLINE;
  return s.mqtt_requested;
}

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

bool request(SceneId id, uint8_t priority, uint16_t duration_s, bool sticky) {
  if (priority > 5) priority = 5;  // FR-2.1 clamp
  // FR-2.3: 0 or absurd duration → default. Cap at the hard TTL.
  uint32_t soft_ms;
  if (sticky) {
    soft_ms = 0;  // sticky: only the hard TTL applies
  } else {
    uint32_t d = (duration_s == 0) ? kDefaultDurationSec : duration_s;
    if (d > 3600u) d = 3600u;  // capped by FR-2.4 anyway
    soft_ms = d * 1000u;
  }

  bool accepted = false;
  mutex_enter_blocking(&s_mutex);
  // FR-2.1 preemption: a request with strictly-lower priority than
  // the active Director scene is dropped. Equal-priority is accepted
  // (latest-wins) per REQUIREMENTS §9 default.
  if (priority < s_state.mqtt_priority) {
    accepted = false;
  } else if (s_state.mqtt_requested == id
          && s_state.mqtt_priority  == priority
          && s_state.sticky         == sticky) {
    // Same logical request — refresh deadlines (Director re-asserting)
    // but don't wake the renderer.
    const uint32_t now_ms = static_cast<uint32_t>(::millis());
    s_state.expires_at_ms  = sticky ? 0u : (now_ms + soft_ms);
    s_state.hard_ttl_at_ms = now_ms + kHardTtlMs;
    accepted = true;
  } else {
    const SceneId  before = resolve(s_state);
    const uint32_t now_ms = static_cast<uint32_t>(::millis());
    s_state.mqtt_requested = id;
    s_state.mqtt_priority  = priority;
    s_state.sticky         = sticky;
    s_state.expires_at_ms  = sticky ? 0u : (now_ms + soft_ms);
    s_state.hard_ttl_at_ms = now_ms + kHardTtlMs;
    if (resolve(s_state) != before) s_state.dirty = true;
    accepted = true;
  }
  mutex_exit(&s_mutex);
  return accepted;
}

void tick(uint32_t now_ms) {
  // FR-2.3 / FR-2.4: revert to the default scene when either deadline
  // has passed. "Default" is the giant clock at priority 0 (FR-9.4)
  // so any future Director request beats it. Skip if we're already
  // sitting on the default at priority 0 — nothing to revert.
  mutex_enter_blocking(&s_mutex);
  const bool soft_due = (s_state.expires_at_ms != 0u)
                     && static_cast<int32_t>(now_ms - s_state.expires_at_ms) >= 0;
  const bool hard_due = static_cast<int32_t>(now_ms - s_state.hard_ttl_at_ms) >= 0;
  const bool already_default = (s_state.mqtt_requested == SceneId::CLOCK)
                            && (s_state.mqtt_priority  == 0)
                            && (!s_state.sticky);
  if ((soft_due || hard_due) && !already_default) {
    const SceneId before = resolve(s_state);
    s_state.mqtt_requested = SceneId::CLOCK;
    s_state.mqtt_priority  = 0;
    s_state.sticky         = false;
    s_state.expires_at_ms  = 0u;            // default has no soft deadline
    s_state.hard_ttl_at_ms = now_ms + kHardTtlMs;
    if (resolve(s_state) != before) s_state.dirty = true;
  }
  mutex_exit(&s_mutex);
}

void set_night_active(bool active) {
  mutex_enter_blocking(&s_mutex);
  if (s_state.night_active != active) {
    const SceneId before = resolve(s_state);
    s_state.night_active = active;
    if (resolve(s_state) != before) s_state.dirty = true;
  }
  mutex_exit(&s_mutex);
}

void set_thermal_active(bool active) {
  mutex_enter_blocking(&s_mutex);
  if (s_state.thermal_active != active) {
    const SceneId before = resolve(s_state);
    s_state.thermal_active = active;
    if (resolve(s_state) != before) s_state.dirty = true;
  }
  mutex_exit(&s_mutex);
}

void set_offline_active(bool active) {
  mutex_enter_blocking(&s_mutex);
  if (s_state.offline_active != active) {
    const SceneId before = resolve(s_state);
    s_state.offline_active = active;
    if (resolve(s_state) != before) s_state.dirty = true;
  }
  mutex_exit(&s_mutex);
}

void set_splash_active(bool active) {
  mutex_enter_blocking(&s_mutex);
  if (s_state.splash_active != active) {
    const SceneId before = resolve(s_state);
    s_state.splash_active = active;
    if (resolve(s_state) != before) s_state.dirty = true;
  }
  mutex_exit(&s_mutex);
}

void clear_sticky() {
  // FR-2.2: clear_sticky only affects scenes that won't auto-expire.
  // For non-sticky scenes, tick() already handles revert; doing
  // anything here would race the Director's own duration intent.
  mutex_enter_blocking(&s_mutex);
  if (s_state.sticky) {
    const SceneId before = resolve(s_state);
    const uint32_t now_ms = static_cast<uint32_t>(::millis());
    s_state.mqtt_requested = SceneId::CLOCK;
    s_state.mqtt_priority  = 0;
    s_state.sticky         = false;
    s_state.expires_at_ms  = 0u;            // default: no soft deadline
    s_state.hard_ttl_at_ms = now_ms + kHardTtlMs;
    if (resolve(s_state) != before) s_state.dirty = true;
  }
  mutex_exit(&s_mutex);
}

bool take_pending(SceneId* out) {
  bool    had;
  SceneId id;
  mutex_enter_blocking(&s_mutex);
  had = s_state.dirty;
  id  = resolve(s_state);
  s_state.dirty = false;
  mutex_exit(&s_mutex);
  if (had && out) *out = id;
  return had;
}

SceneId current() {
  SceneId id;
  mutex_enter_blocking(&s_mutex);
  id = s_state.current;
  mutex_exit(&s_mutex);
  return id;
}

void mark_current(SceneId id) {
  mutex_enter_blocking(&s_mutex);
  s_state.current = id;
  mutex_exit(&s_mutex);
}

// Wire-format strings come from the Director (HA) per the §6 Scene
// Registry. Keep this table append-only and in lockstep with SceneId.
// strcmp on a tiny fixed table is faster and lighter than any hash
// for N < ~16 entries — and it stays static-allocation-clean (NFR-2.2).
namespace {
struct IdMapping {
  const char* name;
  SceneId     id;
};
constexpr IdMapping kIdMap[] = {
  { "boot",         SceneId::BOOT         },
  { "clock",        SceneId::CLOCK        },
  { "color_cycle",  SceneId::COLOR_CYCLE  },
  { "text_demo",    SceneId::TEXT_DEMO    },
  { "bg_starfield", SceneId::BG_STARFIELD },
  { "bg_parallax",  SceneId::BG_PARALLAX  },
  { "bg_nebula",    SceneId::BG_NEBULA    },
  { "bg_bitmap",    SceneId::BG_BITMAP    },
  { "bg_image",     SceneId::BG_IMAGE     },
  { "gfx_test",      SceneId::GFX_TEST      },
  { "sky_timelapse", SceneId::SKY_TIMELAPSE },
  { "iss_pass",      SceneId::ISS_PASS      },
  { "moon_phase",    SceneId::MOON_PHASE    },
  { "jupiter_visibility", SceneId::JUPITER_VISIBILITY },
  { "constellation_now",  SceneId::CONSTELLATION_NOW  },
};
}  // namespace

bool id_from_string(const char* s, SceneId* out) {
  if (s == nullptr || s[0] == '\0' || out == nullptr) return false;
  for (const auto& row : kIdMap) {
    if (strcmp(row.name, s) == 0) {
      *out = row.id;
      return true;
    }
  }
  return false;
}

}  // namespace scene_state

// Implementation of include/scene_state.h. See header for the IPC model.
//
// All access goes through the mutex; copy out under lock, work outside.
// (CODING_PRACTICES §3)

#include "scene_state.h"

#include <string.h>

#include <pico/mutex.h>

namespace scene_state {

namespace {

struct State {
  SceneId mqtt_requested = SceneId::CLOCK;  // Director intent (FR-1)
  SceneId current        = SceneId::BOOT;   // last marked by renderer
  bool    night_active   = false;           // FR-7.2 firmware override
  bool    thermal_active = false;           // FR-7.3 firmware override (highest priority)
  bool    dirty          = true;            // resolved-state changed since last take_pending
};

State   s_state;
mutex_t s_mutex;

// Resolution rule — firmware-owned safety/ambient overrides preempt
// Director intent. Mirror this in scene_state.h's docblock when the
// priority list changes. (FR-7.5: thermal > night > director)
SceneId resolve(const State& s) {
  if (s.thermal_active) return SceneId::THERMAL_SAFE;
  if (s.night_active)   return SceneId::NIGHT;
  return s.mqtt_requested;
}

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void request(SceneId id) {
  mutex_enter_blocking(&s_mutex);
  if (s_state.mqtt_requested != id) {
    const SceneId before = resolve(s_state);
    s_state.mqtt_requested = id;
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

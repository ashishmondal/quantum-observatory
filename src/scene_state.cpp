// Implementation of include/scene_state.h. See header for the IPC model.
//
// All access goes through the mutex; copy out under lock, work outside.
// (CODING_PRACTICES §3)

#include "scene_state.h"

#include <string.h>

#include <Arduino.h>     // millis() — used to stamp expiry deadlines
#include <pico/mutex.h>

#include "seq_snapshot.h"

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

// Phase D.4 / FR-16.7 — the per-frame Core 1 read path goes through
// this seqlock-published snapshot, NOT through s_mutex. Core 0
// continues to use the mutex for write-write coordination (MQTT
// callback vs. tick() vs. sensor pollers); each writer republishes the
// snapshot at the end of its critical section so Core 1 always sees a
// monotonic, torn-free view of "what scene + which overrides".
//
// resolved_seq is bumped only when the resolved Director scene id
// actually changes — so take_pending()'s contract ("true exactly once
// per effective state change") collapses to "my last_seen advanced".
// Override-only flips republish the snapshot but leave resolved_seq
// alone, which is correct: SafetyOverlayLayer reads them every frame
// regardless.
struct Snapshot {
  uint32_t resolved_seq;     // monotonic; bumped on resolved-id change only
  SceneId  resolved;
  bool     splash_active;
  bool     thermal_active;
  bool     night_active;
  bool     offline_active;
  uint8_t  _pad[3];          // explicit POD padding
};

SeqSnapshot<Snapshot> s_pub;
uint32_t s_resolved_seq = 0;       // writer-side counter (mutex-guarded)
SceneId  s_last_resolved = SceneId::CLOCK;  // tracks resolve() of last publish

// Caller MUST hold s_mutex. Computes the public snapshot from s_state
// and publishes via the seqlock so Core 1 picks it up lock-free.
void publish_locked() {
  const SceneId now_resolved = s_state.mqtt_requested;  // == resolve(s_state)
  if (now_resolved != s_last_resolved) {
    ++s_resolved_seq;
    s_last_resolved = now_resolved;
  }
  Snapshot snap{};
  snap.resolved_seq   = s_resolved_seq;
  snap.resolved       = now_resolved;
  snap.splash_active  = s_state.splash_active;
  snap.thermal_active = s_state.thermal_active;
  snap.night_active   = s_state.night_active;
  snap.offline_active = s_state.offline_active;
  s_pub.publish(snap);
}

// Resolution rule — the dispatcher's "active scene" is the Director's
// last request; firmware-owned overrides (NIGHT/THERMAL/OFFLINE/SPLASH)
// are no longer modeled as scene-id swaps here. Phase D.3 (FR-16.2)
// moved them to compositor overlay layers that read the *_active flags
// directly each frame, so the underlying Director scene continues to
// render and animate behind any safety overlay (and resumes from where
// it was when the overlay clears, no scene re-init).
//
// The flags are still stored in this struct because:
//   - Core 0 (sensor polls, MQTT link state) is the natural writer; the
//     overlay layer running on Core 1 needs a mutex-protected reader.
//   - Centralized state keeps the "which override wins" priority list
//     in one place (LookupOverrides::pick); the overlay just consults.
SceneId resolve(const State& s) {
  return s.mqtt_requested;
}

}  // namespace

void init() {
  mutex_init(&s_mutex);
  // Seed the published snapshot so Core 1's first take_pending() /
  // read_overrides() can't observe an all-zero "never published" state.
  // Initial resolved_seq = 1 means the boot-time CLOCK request that
  // follows in setup() will bump it to >=2 — still distinct from a
  // freshly-default-constructed reader's last_seen of 0.
  mutex_enter_blocking(&s_mutex);
  s_resolved_seq  = 1;
  s_last_resolved = s_state.mqtt_requested;
  publish_locked();
  mutex_exit(&s_mutex);
}

bool request(SceneId id, uint8_t priority, uint16_t duration_s, bool sticky,
             bool user_intent) {
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
  //
  // EXCEPTION: user-initiated requests (IR remote dispatch, MQTT
  // observatory/scene topic) bypass the priority gate. Rationale:
  // sticky high-priority firmware-initiated scenes (the ISS
  // visibility auto-switch lands at priority 4 sticky) must NOT
  // lock the user out of normal scene navigation. The user's
  // explicit ▼/▲/Back press is always honoured; the auto-switch
  // simply doesn't re-fire until the next visibility rising edge.
  if (!user_intent && priority < s_state.mqtt_priority) {
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
  if (accepted) publish_locked();
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
    publish_locked();
  }
  mutex_exit(&s_mutex);
}

namespace {

// Common body of every set_*_active() override-flag setter. The four
// flags differ only in which `bool State::*` they target, so a
// pointer-to-member parameter collapses ~30 lines of
// copy-pasted lock/check/publish into one place. Compiles to the
// same code as the original four functions because the member
// pointer is always a compile-time constant at the call site.
void set_override_active(bool State::* flag, bool active) {
  mutex_enter_blocking(&s_mutex);
  if (s_state.*flag != active) {
    const SceneId before = resolve(s_state);
    s_state.*flag = active;
    if (resolve(s_state) != before) s_state.dirty = true;
    publish_locked();
  }
  mutex_exit(&s_mutex);
}

}  // namespace

void set_night_active  (bool a) { set_override_active(&State::night_active,   a); }
void set_thermal_active(bool a) { set_override_active(&State::thermal_active, a); }
void set_offline_active(bool a) { set_override_active(&State::offline_active, a); }
void set_splash_active (bool a) { set_override_active(&State::splash_active,  a); }

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
    publish_locked();
  }
  mutex_exit(&s_mutex);
}

bool take_pending(SceneId* out) {
  // FR-16.7 / phase D.4: lock-free seqlock read on the per-frame Core 1
  // path. Reader-side state is a static last_seen counter — only Core 1
  // calls take_pending(), so a single-instance static is correct (no
  // re-entrancy). "Effective state change" collapses to "resolved_seq
  // advanced since my last call". Override-only flips republish the
  // snapshot but don't bump resolved_seq, so the dispatcher doesn't
  // wake on a NIGHT/THERMAL toggle (those are read directly by the
  // SafetyOverlayLayer via read_overrides()).
  static uint32_t last_seen_resolved_seq = 0;
  Snapshot snap;
  s_pub.read(&snap);
  if (snap.resolved_seq == last_seen_resolved_seq) return false;
  last_seen_resolved_seq = snap.resolved_seq;
  if (out) *out = snap.resolved;
  return true;
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

void read_overrides(bool* splash, bool* thermal, bool* night, bool* offline) {
  // FR-16.7 / phase D.4: lock-free seqlock read. Core 1's overlay layer
  // calls this every frame; routing through the mutex would expose the
  // hot path to Core 0 writer pressure (the failure mode this seqlock
  // exists to eliminate). Snapshot is published atomically by every
  // set_*_active() writer.
  Snapshot snap;
  s_pub.read(&snap);
  if (splash)  *splash  = snap.splash_active;
  if (thermal) *thermal = snap.thermal_active;
  if (night)   *night   = snap.night_active;
  if (offline) *offline = snap.offline_active;
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
  { "iss_pass",      SceneId::ISS_PASS      },
  { "moon_phase",    SceneId::MOON_PHASE    },
  { "jupiter_visibility", SceneId::JUPITER_VISIBILITY },
  { "constellation_now",  SceneId::CONSTELLATION_NOW  },
  { "ir_test",            SceneId::IR_TEST            },
  { "font_demo",          SceneId::FONT_DEMO          },
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

const char* string_from_id(SceneId id) {
  for (const auto& row : kIdMap) {
    if (row.id == id) return row.name;
  }
  return "unknown";
}

}  // namespace scene_state

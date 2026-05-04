// Implementation of include/moon_state.h.

#include "moon_state.h"

#include <string.h>

#include <pico/mutex.h>

namespace moon_state {

namespace {

struct State {
  bool     have      = false;
  uint32_t set_at_ms = 0;
  float    phase_frac = 0.0f;
  uint8_t  illum_pct  = 0;
  uint16_t age_d      = 0;
  char     name[12]   = {0};
};

State   s_state;
mutex_t s_mutex;

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void set_from_mqtt(float phase_frac, uint8_t illum_pct, uint16_t age_d,
                   const char* name, uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  s_state.have       = true;
  s_state.set_at_ms  = now_ms;
  s_state.phase_frac = phase_frac;
  s_state.illum_pct  = illum_pct;
  s_state.age_d      = age_d;
  if (name != nullptr) {
    strncpy(s_state.name, name, sizeof(s_state.name) - 1);
    s_state.name[sizeof(s_state.name) - 1] = '\0';
  } else {
    s_state.name[0] = '\0';
  }
  mutex_exit(&s_mutex);
}

bool get(uint32_t now_ms, Snapshot* out) {
  bool fresh = false;
  mutex_enter_blocking(&s_mutex);
  if (s_state.have && (now_ms - s_state.set_at_ms) < kFreshMs) {
    fresh = true;
    if (out != nullptr) {
      out->valid      = true;
      out->phase_frac = s_state.phase_frac;
      out->illum_pct  = s_state.illum_pct;
      out->age_d      = s_state.age_d;
      memcpy(out->name, s_state.name, sizeof(out->name));
    }
  } else if (out != nullptr) {
    out->valid = false;
    out->name[0] = '\0';
  }
  mutex_exit(&s_mutex);
  return fresh;
}

}  // namespace moon_state

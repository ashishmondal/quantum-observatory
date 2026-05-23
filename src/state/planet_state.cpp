// Implementation of include/state/planet_state.h.

#include "planet_state.h"

#include <string.h>

#include <pico/mutex.h>

namespace planet_state {

namespace {

struct State {
  bool     have                = false;
  uint32_t set_at_ms           = 0;
  char     name[kNameCap]      = {0};
  int16_t  bearing_deg         = 0;
  int8_t   elevation_deg       = 0;
  uint8_t  constellation_index = 0;
};

State   s_state;
mutex_t s_mutex;

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void set_from_mqtt(const char* name,
                   int16_t bearing_deg, int8_t elevation_deg,
                   uint8_t constellation_index,
                   uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  s_state.have                = true;
  s_state.set_at_ms           = now_ms;
  s_state.bearing_deg         = bearing_deg;
  s_state.elevation_deg       = elevation_deg;
  s_state.constellation_index = constellation_index;
  // Defensive copy — mqtt_link guarantees name is NUL-terminated and
  // ≤ kNameCap-1 chars, but truncate-with-NUL here so a future caller
  // can't tear the snapshot.
  size_t i = 0;
  if (name != nullptr) {
    while (i + 1 < kNameCap && name[i] != '\0') {
      s_state.name[i] = name[i];
      ++i;
    }
  }
  s_state.name[i] = '\0';
  mutex_exit(&s_mutex);
}

bool get(uint32_t now_ms, Snapshot* out) {
  bool fresh = false;
  mutex_enter_blocking(&s_mutex);
  if (s_state.have && (now_ms - s_state.set_at_ms) < kFreshMs) {
    fresh = true;
    if (out != nullptr) {
      out->valid               = true;
      memcpy(out->name, s_state.name, kNameCap);
      out->bearing_deg         = s_state.bearing_deg;
      out->elevation_deg       = s_state.elevation_deg;
      out->constellation_index = s_state.constellation_index;
      out->set_at_ms           = s_state.set_at_ms;
    }
  } else if (out != nullptr) {
    out->valid   = false;
    out->name[0] = '\0';
  }
  mutex_exit(&s_mutex);
  return fresh;
}

}  // namespace planet_state

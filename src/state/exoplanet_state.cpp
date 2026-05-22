// Implementation of include/state/exoplanet_state.h.

#include "exoplanet_state.h"

#include <string.h>

#include <pico/mutex.h>

namespace exoplanet_state {

namespace {

struct State {
  bool     have                     = false;
  uint32_t set_at_ms                = 0;
  uint32_t total_count              = 0;
  bool     have_added_recent        = false;
  int16_t  added_recent             = 0;
  char     nearest_name[kNameCap]   = {0};
  bool     have_nearest_distance    = false;
  uint16_t nearest_distance_ly_x10  = 0;
};

State   s_state;
mutex_t s_mutex;

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void set_from_mqtt(uint32_t total_count,
                   bool have_added_recent, int16_t added_recent,
                   const char* nearest_name,
                   bool have_nearest_distance,
                   uint16_t nearest_distance_ly_x10,
                   uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  s_state.have                    = true;
  s_state.set_at_ms               = now_ms;
  s_state.total_count             = total_count;
  s_state.have_added_recent       = have_added_recent;
  s_state.added_recent            = have_added_recent ? added_recent : 0;
  s_state.have_nearest_distance   = have_nearest_distance;
  s_state.nearest_distance_ly_x10 =
      have_nearest_distance ? nearest_distance_ly_x10 : 0;

  // Defensive copy with explicit length cap (caller is validated but
  // we'd rather not depend on that for a NUL-terminated buffer).
  if (nearest_name == nullptr) {
    s_state.nearest_name[0] = '\0';
  } else {
    size_t i = 0;
    while (i < kNameCap - 1 && nearest_name[i] != '\0') {
      s_state.nearest_name[i] = nearest_name[i];
      ++i;
    }
    s_state.nearest_name[i] = '\0';
  }
  mutex_exit(&s_mutex);
}

bool get(uint32_t now_ms, Snapshot* out) {
  bool fresh = false;
  mutex_enter_blocking(&s_mutex);
  if (s_state.have && (now_ms - s_state.set_at_ms) < kFreshMs) {
    fresh = true;
    if (out != nullptr) {
      out->valid                   = true;
      out->total_count             = s_state.total_count;
      out->have_added_recent       = s_state.have_added_recent;
      out->added_recent            = s_state.added_recent;
      memcpy(out->nearest_name, s_state.nearest_name, kNameCap);
      out->nearest_name[kNameCap - 1] = '\0';
      out->have_nearest_distance   = s_state.have_nearest_distance;
      out->nearest_distance_ly_x10 = s_state.nearest_distance_ly_x10;
      out->set_at_ms               = s_state.set_at_ms;
    }
  } else if (out != nullptr) {
    out->valid           = false;
    out->nearest_name[0] = '\0';
  }
  mutex_exit(&s_mutex);
  return fresh;
}

}  // namespace exoplanet_state

// Implementation of include/jupiter_state.h.

#include "jupiter_state.h"

#include <pico/mutex.h>

namespace jupiter_state {

namespace {

struct State {
  bool     have             = false;
  uint32_t set_at_ms        = 0;
  int16_t  bearing_deg      = 0;
  int8_t   elevation_deg    = 0;
  bool     have_magnitude   = false;
  int16_t  magnitude_x10    = 0;
  bool     have_distance    = false;
  uint16_t distance_au_x10  = 0;  bool     have_constellation   = false;
  uint8_t  constellation_index  = 0;};

State   s_state;
mutex_t s_mutex;

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void set_from_mqtt(int16_t bearing_deg, int8_t elevation_deg,
                   bool have_magnitude, int16_t magnitude_x10,
                   bool have_distance, uint16_t distance_au_x10,
                   bool have_constellation, uint8_t constellation_index,
                   uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  s_state.have                 = true;
  s_state.set_at_ms            = now_ms;
  s_state.bearing_deg          = bearing_deg;
  s_state.elevation_deg        = elevation_deg;
  s_state.have_magnitude       = have_magnitude;
  s_state.magnitude_x10        = have_magnitude ? magnitude_x10   : 0;
  s_state.have_distance        = have_distance;
  s_state.distance_au_x10      = have_distance ? distance_au_x10 : 0;
  s_state.have_constellation   = have_constellation;
  s_state.constellation_index  = have_constellation ? constellation_index : 0;
  mutex_exit(&s_mutex);
}

bool get(uint32_t now_ms, Snapshot* out) {
  bool fresh = false;
  mutex_enter_blocking(&s_mutex);
  if (s_state.have && (now_ms - s_state.set_at_ms) < kFreshMs) {
    fresh = true;
    if (out != nullptr) {
      out->valid                = true;
      out->bearing_deg          = s_state.bearing_deg;
      out->elevation_deg        = s_state.elevation_deg;
      out->have_magnitude       = s_state.have_magnitude;
      out->magnitude_x10        = s_state.magnitude_x10;
      out->have_distance        = s_state.have_distance;
      out->distance_au_x10      = s_state.distance_au_x10;
      out->have_constellation   = s_state.have_constellation;
      out->constellation_index  = s_state.constellation_index;
      out->set_at_ms            = s_state.set_at_ms;
    }
  } else if (out != nullptr) {
    out->valid = false;
  }
  mutex_exit(&s_mutex);
  return fresh;
}

}  // namespace jupiter_state

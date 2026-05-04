// Implementation of include/iss_state.h.

#include "iss_state.h"

#include <pico/mutex.h>

namespace iss_state {

namespace {

struct State {
  bool     have               = false;
  uint32_t set_at_ms          = 0;
  float    iss_lat_deg        = 0.0f;
  float    iss_lon_deg        = 0.0f;
  uint16_t altitude_km        = 0;
  bool     sunlit             = false;
  uint32_t seconds_until_next = 0;
  bool     have_crew          = false;
  uint8_t  crew_count         = 0;
};

State   s_state;
mutex_t s_mutex;

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void set_from_mqtt(float iss_lat_deg, float iss_lon_deg,
                   uint16_t altitude_km, bool sunlit,
                   uint32_t seconds_until_next,
                   bool have_crew, uint8_t crew_count,
                   uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  s_state.have               = true;
  s_state.set_at_ms          = now_ms;
  s_state.iss_lat_deg        = iss_lat_deg;
  s_state.iss_lon_deg        = iss_lon_deg;
  s_state.altitude_km        = altitude_km;
  s_state.sunlit             = sunlit;
  s_state.seconds_until_next = seconds_until_next;
  s_state.have_crew          = have_crew;
  s_state.crew_count         = have_crew ? crew_count : 0;
  mutex_exit(&s_mutex);
}

bool get(uint32_t now_ms, Snapshot* out) {
  bool fresh = false;
  mutex_enter_blocking(&s_mutex);
  if (s_state.have && (now_ms - s_state.set_at_ms) < kFreshMs) {
    fresh = true;
    if (out != nullptr) {
      out->valid              = true;
      out->iss_lat_deg        = s_state.iss_lat_deg;
      out->iss_lon_deg        = s_state.iss_lon_deg;
      out->altitude_km        = s_state.altitude_km;
      out->sunlit             = s_state.sunlit;
      out->seconds_until_next = s_state.seconds_until_next;
      out->have_crew          = s_state.have_crew;
      out->crew_count         = s_state.crew_count;
      out->set_at_ms          = s_state.set_at_ms;
    }
  } else if (out != nullptr) {
    out->valid = false;
  }
  mutex_exit(&s_mutex);
  return fresh;
}

}  // namespace iss_state

// Implementation of include/constellation_state.h.

#include "constellation_state.h"

#include <pico/mutex.h>

namespace constellation_state {

namespace {

struct State {
  bool     have           = false;
  uint32_t set_at_ms      = 0;
  uint8_t  index          = 0;
  bool     have_highlight = false;
  uint8_t  highlight_star = 0;
};

State   s_state;
mutex_t s_mutex;

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void set_from_mqtt(uint8_t index,
                   bool have_highlight, uint8_t highlight_star,
                   uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  s_state.have           = true;
  s_state.set_at_ms      = now_ms;
  s_state.index          = index;
  s_state.have_highlight = have_highlight;
  s_state.highlight_star = have_highlight ? highlight_star : 0;
  mutex_exit(&s_mutex);
}

bool get(uint32_t now_ms, Snapshot* out) {
  bool fresh = false;
  mutex_enter_blocking(&s_mutex);
  if (s_state.have && (now_ms - s_state.set_at_ms) < kFreshMs) {
    fresh = true;
    if (out != nullptr) {
      out->valid          = true;
      out->index          = s_state.index;
      out->have_highlight = s_state.have_highlight;
      out->highlight_star = s_state.highlight_star;
      out->set_at_ms      = s_state.set_at_ms;
    }
  } else if (out != nullptr) {
    out->valid = false;
  }
  mutex_exit(&s_mutex);
  return fresh;
}

}  // namespace constellation_state

// Implementation of include/state/feeds.h.

#include "feeds.h"

#include <pico/mutex.h>

namespace feeds {

namespace {

template <typename T>
struct Slot {
  T        v{};
  bool     have      = false;
  uint32_t set_at_ms = 0;
};

mutex_t       s_mutex;
Slot<Weather> s_weather;
Slot<Sky>     s_sky;
Slot<Aurora>  s_aurora;
Slot<Gw>      s_gw;
Slot<Artemis> s_artemis;
Slot<Alert>   s_alert;

template <typename T>
void put(Slot<T>& slot, const T& v, uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  slot.v         = v;
  slot.have      = true;
  slot.set_at_ms = now_ms;
  mutex_exit(&s_mutex);
}

template <typename T>
bool take(const Slot<T>& slot, uint32_t now_ms, T* out, uint32_t* age_ms) {
  mutex_enter_blocking(&s_mutex);
  const uint32_t age   = now_ms - slot.set_at_ms;
  const bool     fresh = slot.have && age < slot.v.fresh_ms();
  if (fresh && out != nullptr) *out = slot.v;
  mutex_exit(&s_mutex);
  if (age_ms != nullptr) *age_ms = age;
  return fresh;
}

}  // namespace

void init() { mutex_init(&s_mutex); }

void set(const Weather& v, uint32_t t) { put(s_weather, v, t); }
void set(const Sky& v,     uint32_t t) { put(s_sky,     v, t); }
void set(const Aurora& v,  uint32_t t) { put(s_aurora,  v, t); }
void set(const Gw& v,      uint32_t t) { put(s_gw,      v, t); }
void set(const Artemis& v, uint32_t t) { put(s_artemis, v, t); }
void set(const Alert& v,   uint32_t t) { put(s_alert,   v, t); }

bool get(uint32_t t, Weather* o, uint32_t* a) { return take(s_weather, t, o, a); }
bool get(uint32_t t, Sky* o,     uint32_t* a) { return take(s_sky,     t, o, a); }
bool get(uint32_t t, Aurora* o,  uint32_t* a) { return take(s_aurora,  t, o, a); }
bool get(uint32_t t, Gw* o,      uint32_t* a) { return take(s_gw,      t, o, a); }
bool get(uint32_t t, Artemis* o, uint32_t* a) { return take(s_artemis, t, o, a); }
bool get(uint32_t t, Alert* o,   uint32_t* a) { return take(s_alert,   t, o, a); }

}  // namespace feeds

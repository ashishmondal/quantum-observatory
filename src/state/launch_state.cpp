// Implementation of include/state/launch_state.h.

#include "launch_state.h"

#include <string.h>

#include <pico/mutex.h>

namespace launch_state {

namespace {

struct State {
  bool     have                         = false;
  uint32_t set_at_ms                    = 0;
  int32_t  t0_local_epoch               = 0;
  int32_t  t0_window_close_local_epoch  = 0;
  bool     t0_estimate                  = false;
  int8_t   result                       = -1;
  char     provider[kProviderCap]       = { 0 };
  char     vehicle[kVehicleCap]         = { 0 };
  char     mission[kMissionCap]         = { 0 };
  char     pad_code[kPadCodeCap]        = { 0 };
  char     org[kOrgCap]                 = { 0 };
  char     pad_country[kPadCountryCap]  = { 0 };
  char     description[kDescriptionCap] = { 0 };
};

State   s_state;
mutex_t s_mutex;

// Defensive: null-terminate at cap-1 even if caller forgot. strncpy
// is the right tool here precisely because we want the trailing-NUL
// pad behaviour AND a hard length cap.
void copy_clamped(char* dst, size_t cap, const char* src) {
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap);
  dst[cap - 1] = '\0';
}

}  // namespace

void init() {
  mutex_init(&s_mutex);
}

void set_from_mqtt(int32_t t0_local_epoch, bool t0_estimate,
                   int32_t t0_window_close_local_epoch,
                   int8_t  result,
                   const char* provider,
                   const char* vehicle,
                   const char* mission,
                   const char* pad_code,
                   const char* org,
                   const char* pad_country,
                   const char* description,
                   uint32_t now_ms) {
  mutex_enter_blocking(&s_mutex);
  s_state.have                        = true;
  s_state.set_at_ms                   = now_ms;
  s_state.t0_local_epoch              = t0_local_epoch;
  s_state.t0_window_close_local_epoch = t0_window_close_local_epoch;
  s_state.t0_estimate                 = t0_estimate;
  s_state.result                      = result;
  copy_clamped(s_state.provider,    kProviderCap,    provider);
  copy_clamped(s_state.vehicle,     kVehicleCap,     vehicle);
  copy_clamped(s_state.mission,     kMissionCap,     mission);
  copy_clamped(s_state.pad_code,    kPadCodeCap,     pad_code);
  copy_clamped(s_state.org,         kOrgCap,         org);
  copy_clamped(s_state.pad_country, kPadCountryCap,  pad_country);
  copy_clamped(s_state.description, kDescriptionCap, description);
  mutex_exit(&s_mutex);
}

bool get(uint32_t now_ms, Snapshot* out) {
  bool fresh = false;
  mutex_enter_blocking(&s_mutex);
  if (s_state.have && (now_ms - s_state.set_at_ms) < kFreshMs) {
    fresh = true;
    if (out != nullptr) {
      out->valid                       = true;
      out->t0_local_epoch              = s_state.t0_local_epoch;
      out->t0_window_close_local_epoch = s_state.t0_window_close_local_epoch;
      out->t0_estimate                 = s_state.t0_estimate;
      out->result                      = s_state.result;
      memcpy(out->provider,    s_state.provider,    kProviderCap);
      memcpy(out->vehicle,     s_state.vehicle,     kVehicleCap);
      memcpy(out->mission,     s_state.mission,     kMissionCap);
      memcpy(out->pad_code,    s_state.pad_code,    kPadCodeCap);
      memcpy(out->org,         s_state.org,         kOrgCap);
      memcpy(out->pad_country, s_state.pad_country, kPadCountryCap);
      memcpy(out->description, s_state.description, kDescriptionCap);
      out->set_at_ms             = s_state.set_at_ms;
    }
  } else if (out != nullptr) {
    out->valid = false;
  }
  mutex_exit(&s_mutex);
  return fresh;
}

}  // namespace launch_state

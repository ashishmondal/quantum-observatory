// Implementation of include/time_of_day.h. See header for rationale.
//
// All math is integer (NFR-1.3). Date breakdown lives in a separate
// helper (date_from_local_epoch + weekday/month abbreviations) so the
// hot HH:MM:SS reader path stays branch-free.

#include "time_of_day.h"

#include "ds3231.h"

namespace tod {

namespace {

// Cached snapshot of the most recent successful RTC read. Touched by
// both cores; always accessed with s_mutex held.
struct State {
  bool     valid       = false;  // true iff last poll() succeeded AND OSF clear
  int32_t  local_epoch = 0;      // local-time epoch seconds at read_at_ms
  uint32_t read_at_ms  = 0;      // millis() sampled around the i2c read
};

State      s_state;
mutex_t    s_mutex;

} // namespace

void init() {
  mutex_init(&s_mutex);
}

void poll(uint32_t now_ms) {
  // Do the I²C work OUTSIDE the lock — it's slow (sub-millisecond but
  // still longer than the 3-word state copy). The chip is only touched
  // by Core 0 so there's no concurrent-access concern on the bus.
  int32_t epoch = 0;
  const bool ok  = ds3231::read(&epoch);
  const bool osf = ds3231::oscillator_stopped();
  const bool valid = ok && !osf;

  mutex_enter_blocking(&s_mutex);
  s_state.valid       = valid;
  if (valid) {
    s_state.local_epoch = epoch;
    s_state.read_at_ms  = now_ms;
  }
  mutex_exit(&s_mutex);
}

bool set_from_mqtt(int32_t epoch_utc, int16_t tz_offset_min, uint32_t now_ms) {
  // Convert UTC -> local before persisting; the RTC stores local time
  // (Phase 3.6.3 decision) so reads need no tz state.
  const int32_t local = epoch_utc + static_cast<int32_t>(tz_offset_min) * 60;
  if (!ds3231::write(local)) {
    return false;
  }
  ds3231::clear_oscillator_stopped();
  poll(now_ms);
  return true;
}

Reading now(uint32_t now_ms) {
  // Snapshot under lock, then do the math outside — keeps the lock window
  // to ~3 word copies (CODING_PRACTICES §3 "shortest possible window").
  bool     valid;
  int32_t  local_epoch;
  uint32_t read_at_ms;
  mutex_enter_blocking(&s_mutex);
  valid       = s_state.valid;
  local_epoch = s_state.local_epoch;
  read_at_ms  = s_state.read_at_ms;
  mutex_exit(&s_mutex);

  Reading r{};
  r.valid = valid;
  if (!valid) {
    return r;
  }

  // Wrap-safe delta (CODING_PRACTICES §2). Convert ms→s by integer divide;
  // sub-second smoothing finer than 1 s isn't useful for HH:MM:SS readout.
  const uint32_t delta_ms = now_ms - read_at_ms;
  const int32_t  proj     = local_epoch + static_cast<int32_t>(delta_ms / 1000u);

  // Day-of-second math. uint cast keeps modulo well-defined for negative
  // proj values (pre-1970 — we'd never see one in practice but the cast
  // is free).
  const uint32_t sod = static_cast<uint32_t>(proj) % 86400u;
  r.local_epoch = proj;
  r.hour   = static_cast<uint8_t>(sod / 3600u);
  r.minute = static_cast<uint8_t>((sod % 3600u) / 60u);
  r.second = static_cast<uint8_t>(sod % 60u);
  return r;
}

bool now_hhmm(uint32_t now_ms, uint8_t* h, uint8_t* m) {
  const Reading r = now(now_ms);
  if (!r.valid) {
    return false;
  }
  if (h) *h = r.hour;
  if (m) *m = r.minute;
  return true;
}

// Howard Hinnant's "civil_from_days" — the canonical pure-integer
// gregorian breakdown algorithm. Source:
//   https://howardhinnant.github.io/date_algorithms.html#civil_from_days
// Public domain. Adapted to int32 and our (year, month, day) outputs.
//
// Day 0 of the algorithm == 1970-01-01 (matches Unix epoch); weekday is
// then a one-line modulo since 1970-01-01 was a Thursday.
void date_from_local_epoch(int32_t local_epoch,
                           int16_t* year, uint8_t* month,
                           uint8_t* day,  uint8_t* weekday) {
  // Floor-divide by 86400 to get day count even for negative epochs.
  int32_t days = local_epoch / 86400;
  if (local_epoch < 0 && (local_epoch % 86400) != 0) {
    --days;
  }

  if (weekday) {
    // 1970-01-01 was Thursday (=4 with Sun=0). Use Euclidean mod so the
    // result is always in [0,7).
    int32_t w = (days + 4) % 7;
    if (w < 0) w += 7;
    *weekday = static_cast<uint8_t>(w);
  }

  // Shift epoch from 1970-01-01 to 0000-03-01 (Hinnant's anchor).
  days += 719468;
  const int32_t era = (days >= 0 ? days : days - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(days - era * 146097);             // [0, 146096]
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int32_t  y   = static_cast<int32_t>(yoe) + era * 400;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
  const uint32_t mp  = (5 * doy + 2) / 153;                                    // [0, 11]
  const uint32_t d   = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
  const uint32_t m   = mp < 10 ? mp + 3 : mp - 9;                              // [1, 12]

  if (year)  *year  = static_cast<int16_t>(y + (m <= 2 ? 1 : 0));
  if (month) *month = static_cast<uint8_t>(m);
  if (day)   *day   = static_cast<uint8_t>(d);
}

const char* weekday_abbrev(uint8_t weekday) {
  static const char* const NAMES[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
  };
  return NAMES[weekday < 7 ? weekday : 0];
}

const char* month_abbrev(uint8_t month) {
  static const char* const NAMES[13] = {
    "---", // index 0 unused; months are 1-based
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
  };
  return NAMES[(month >= 1 && month <= 12) ? month : 0];
}

} // namespace tod

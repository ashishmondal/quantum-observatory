// Authoritative time-of-day state.
//
// FR-9.5: the on-board DS3231 RTC is the SINGLE read path for the wall
// clock. MQTT (Phase 5.6) is the *correction* path — it writes the RTC
// via tod::set_from_mqtt(), which is then read back like any other
// value. Nothing in this module ever consults MQTT, NTP, or
// millis()-since-boot as a time source — millis() is used only for
// sub-second smoothing between RTC polls.
//
// FR-9.6: until the RTC has been read at least once AND its
// oscillator-stop flag is clear, readers get back valid=false and
// SHOULD render "--:--" — never a guessed time.
//
// Cross-core: tod::poll() (Core 0) refreshes the cached snapshot from
// the RTC; tod::now() (Core 1, render) reads the snapshot under a
// mutex_t. The mutex is required because more than one field crosses
// the boundary (epoch + read_at + valid), which the volatile-bool
// pattern from 4.1 cannot cover. (rewired in phase 3.6.2)

#pragma once

#include <Arduino.h>
#include <pico/mutex.h>
#include <stdint.h>

namespace tod {

// Local seconds since the Unix epoch, projected to "now" using a millis()
// delta from the moment of the most recent RTC read. Always derived
// — never stored — to avoid drift accumulation across calls.
struct Reading {
  bool     valid;        // false until first poll() succeeds AND OSF clear
  uint8_t  hour;         // 0..23 in local time
  uint8_t  minute;       // 0..59
  uint8_t  second;       // 0..59
  int32_t  local_epoch;  // RTC epoch + millis-smoothing, seconds (debug)
  int16_t  tz_offset_min;// minutes east of UTC, signed. Set by
                         // tod::set_from_mqtt(); zero (== UTC) until
                         // HA's first observatory/time push lands.
};

// Initialise the mutex. Call once from setup() before any reader spins.
// Does NOT touch the RTC; call ds3231::begin() separately first.
void init();

// Refresh the cached snapshot from the DS3231. Cheap (~one I²C
// transaction) but call at most ~1 Hz from Core 0; readers smooth
// sub-second resolution via millis() between polls. Sets valid=false
// if the read fails or the oscillator-stop flag is set (FR-9.6).
void poll(uint32_t now_ms);

// Like poll() but rejects readings that disagree with the projected
// epoch by more than max_jump_s. Returns true on accept (cache
// updated), false on reject (cache untouched). Use for low-cadence
// polling where a glitched read shouldn't poison the cache for hours.
// On the first-ever successful poll (cache previously invalid) the
// value is always accepted regardless of jump.
bool poll_validated(uint32_t now_ms, uint32_t max_jump_s);

// MQTT correction path (Phase 5.6). Writes the supplied wall-clock
// into the RTC, clears the oscillator-stop flag, then re-polls so
// callers see the new value immediately. epoch_utc = Unix seconds,
// tz_offset_min = local-UTC in minutes (e.g. +330 for IST). The RTC
// stores LOCAL time per the Phase 3.6.3 decision; UTC->local
// conversion happens here. Returns true on success.
bool set_from_mqtt(int32_t epoch_utc, int16_t tz_offset_min, uint32_t now_ms);

// Reader (any core). Returns a snapshot projected to the supplied
// now_ms. Returns valid=false until poll() has succeeded at least once
// against an RTC with a running oscillator.
Reading now(uint32_t now_ms);

// Convenience for chrome / scenes that only want HH:MM. Returns true if
// the time is valid; on false the caller MUST render "--:--".
bool now_hhmm(uint32_t now_ms, uint8_t* h, uint8_t* m);

// Civil date breakdown of a local epoch (seconds since 1970-01-01 00:00
// in the device's local timezone). Pure integer math — uses Howard
// Hinnant's days-from-civil algorithm. Outputs are independent so a
// caller (e.g. the giant clock scene) can ignore year if it doesn't
// display it.
//   year:    full year (e.g. 2026)
//   month:   1..12
//   day:     1..31
//   weekday: 0..6, 0 = Sunday  (matches strftime %w)
//
// Caller responsibility: only invoke when tod::now() returned valid=true.
void date_from_local_epoch(int32_t local_epoch,
                           int16_t* year, uint8_t* month,
                           uint8_t* day,  uint8_t* weekday);

// Three-letter, uppercase abbreviations matching the FR-9.4 sample
// "WED 01 MAY". Pointers are static PROGMEM-style strings — never free,
// safe to use as `const char*` indefinitely.
const char* weekday_abbrev(uint8_t weekday);  // 0..6, 0 = Sunday
const char* month_abbrev(uint8_t month);      // 1..12

} // namespace tod

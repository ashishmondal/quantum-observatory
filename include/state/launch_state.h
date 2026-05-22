// observatory/launch — raw HA pass-through of the next scheduled
// rocket launch for the `launch_countdown` scene (FR-14.6, phase L).
//
// Same Director/Cinematographer split as iss_state / jupiter_state /
// constellation_state: HA polls a public launch feed (Rocket Launch
// Live "fdo" tier in v1 — `https://fdo.rocketlaunch.live/json/
// launches/next/5`), resolves provider/pad abbreviations from
// operator-editable dicts, and re-emits the snapshot verbatim. The
// firmware does only integer T-minus arithmetic against the live RTC
// epoch (NFR-1.3 — no float in render loops) and decides which of
// five render regimes (T-Nd HHh / T-HHh MMm / T-MM:SS / blinking
// T-SS / LIVE) applies.
//
// Wire fields (see docs/MQTT_TOPICS.md `observatory/launch`):
//   t0_local_epoch          — required, HA-local-zone wall seconds
//                              since 1970 (NOT UTC). HA's pyscript
//                              publisher converts the upstream UTC
//                              timestamp into the same frame the
//                              device's RTC reads back via tod::now()
//                              so the scene reduces to a pure
//                              subtraction with zero tz state on the
//                              firmware. Best available T-zero
//                              (confirmed, else win_open, else
//                              midday-local est_date).
//   t0_estimate             — required bool, true when t0_local_epoch
//                              came from win_open or est_date
//                              (surfaces on-panel as "NET " prefix).
//   t0_window_close_local_epoch
//                           — optional, 0 = absent. Same local-epoch
//                              frame. When present the "LIVE" regime
//                              fires while
//                              now ∈ [t0_local_epoch,
//                                       t0_window_close_local_epoch].
//   provider                — required, ≤ 11 chars + NUL (HA's pre-
//                              abbreviated tag, e.g. "SX" "RKL").
//   vehicle                 — required, ≤ 11 chars + NUL.
//   mission                 — required, ≤ 13 chars + NUL.
//   pad_code                — required, ≤ 4 chars + NUL.
//   result                  — optional, -1 scheduled (default),
//                              0=failure 1=success 2=partial. The
//                              publisher SHOULD filter result != -1
//                              upstream; this field exists so the
//                              firmware can defensively skip a stale
//                              entry that slipped through.//
// Cross-core: writer is Core 0 (MQTT callback), reader is Core 1
// (render). Multi-field snapshot → real mutex_t per
// CODING_PRACTICES §3.
//
// Freshness: 4 h ceiling (`kFreshMs`) — covers HA's hourly poll
// cadence + slippage + a single missed poll. Beyond that the scene
// falls back to `WAIT` rather than ticking down a stale date. A
// snapshot whose t0_epoch has elapsed by more than 1 h with no
// t0_window_close_epoch is treated as expired by the scene as well
// (fail-closed: HA's next poll publishes the replacement).

#pragma once

#include <stdint.h>

namespace launch_state {

// On-wire string caps + 1 byte NUL terminator. Match the field widths
// documented in MQTT_TOPICS.md / FR-4.4 (Director-side truncation).
constexpr uint8_t  kProviderCap    = 12;
constexpr uint8_t  kVehicleCap     = 12;
constexpr uint8_t  kMissionCap     = 14;
constexpr uint8_t  kPadCodeCap     = 5;
// Friendlier longer fields surfaced on the typewriter info row.
// `org` is the launch provider's full name ("SPACEX", "ROCKET LAB")
// and `pad_country` is the country (or "STATE, USA" for US sites)
// derived from the last comma-segment of `pad.location.name`. Sized
// so the 4 px/char TomThumb row can fit ~16 chars of value beside a
// ~3-char label without overflowing the 64 px panel width.
constexpr uint8_t  kOrgCap         = 21;
constexpr uint8_t  kPadCountryCap  = 21;
// Mission description from the LL2 `mission.description` field, HA-
// trimmed (control chars stripped, whitespace collapsed, ASCII-fold)
// and length-clipped before publish. Sized to give the operator a
// meaningful blurb to read while waiting on a long countdown
// (~6 lines of prose at 6 px/char = ~40 s scroll cycle on the 64 px
// panel) without blowing the firmware's MQTT receive buffer.
constexpr uint16_t kDescriptionCap = 241;

struct Snapshot {
  bool     valid;                       // false until first set_from_mqtt OR stale

  int32_t  t0_local_epoch;              // HA-local wall epoch (NOT UTC)
  int32_t  t0_window_close_local_epoch; // 0 = absent
  bool     t0_estimate;                 // true => render "NET " prefix
  int8_t   result;                      // -1 scheduled / 0 fail / 1 ok / 2 partial

  char     provider[kProviderCap]; // NUL-terminated
  char     vehicle[kVehicleCap];
  char     mission[kMissionCap];
  char     pad_code[kPadCodeCap];
  char     org[kOrgCap];               // full provider name; empty when absent
  char     pad_country[kPadCountryCap]; // country or "STATE, USA"; empty when absent
  char     description[kDescriptionCap];  // empty string when absent

  uint32_t set_at_ms;              // millis() when the push was applied
};

// 4 h. Covers HA's hourly poll cadence + slippage + one missed poll.
// Past freshness the scene renders "WAIT" rather than fabricating a
// stale countdown — same fail-closed discipline as iss_state's
// `kFreshMs = 1 h` (the ISS sky position moves ~28 000 km/h so its
// window is tighter; launch t0 is a constant of the universe until
// HA reschedules, so we get a much larger window).
constexpr uint32_t kFreshMs = 4u * 60u * 60u * 1000u;

// One-time mutex init. Call from setup() before either core spins.
void init();

// MQTT writer. Validation + range-clamping happens in mqtt_link
// before this is called; the string args MUST already be ≤ cap and
// NUL-terminated. `t0_window_close_local_epoch == 0` means "no
// window" (the LIVE regime never fires). Both epoch args are in
// HA-local-zone wall seconds (NOT UTC) — see header comment.
void set_from_mqtt(int32_t t0_local_epoch, bool t0_estimate,
                   int32_t t0_window_close_local_epoch,
                   int8_t  result,
                   const char* provider,
                   const char* vehicle,
                   const char* mission,
                   const char* pad_code,
                   const char* org,          // may be nullptr/""
                   const char* pad_country,  // may be nullptr/""
                   const char* description,  // may be nullptr/""
                   uint32_t now_ms);

// Reader. Returns true with a fresh snapshot if a value has been
// pushed in the last kFreshMs; false otherwise (caller renders a
// "WAIT" placeholder). Note that "fresh on the wire" doesn't imply
// "T-minus is still meaningful" — the scene also drops to WAIT when
// (t_minus < -3600 && t0_window_close_local_epoch == 0).
bool get(uint32_t now_ms, Snapshot* out);

}  // namespace launch_state

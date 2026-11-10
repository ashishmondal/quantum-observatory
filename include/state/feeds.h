// HA-pushed data feeds for the teen-profile scenes (weather, sky_now,
// alert, aurora, black_holes, artemis).
//
// One typed struct per MQTT topic, all stored behind a single mutex
// (CODING_PRACTICES §3 — multi-field cross-core state). Writer is
// Core 0 (mqtt_link handlers, already validated); readers are Core 1
// scenes, which copy a snapshot per frame. Each struct carries its
// own freshness window via fresh_ms(); a stale or never-set feed
// reads back as `false` so scenes can show WAIT / offline fallbacks.

#pragma once

#include <stdint.h>

namespace feeds {

// observatory/weather — today's numbers + tonight's cloud cover.
struct Weather {
  int16_t temp_f;              // now
  int16_t hi_f;                // today's high
  int16_t lo_f;                // today's low (or overnight low after noon)
  uint8_t precip_pct;          // max precipitation probability today
  uint8_t cloud_tonight_pct;   // mean cloud cover 19:00–23:00
  uint8_t wind_mph;
  char    cond[12];            // short condition, e.g. "PARTLY CLDY"
  uint32_t fresh_ms() const { return 3u * 3600u * 1000u; }
};

// observatory/sky — bright objects above the horizon right now.
struct SkyObj {
  char    name[9];             // "VENUS", "MOON", "SIRIUS"
  uint16_t az_deg;             // 0..359
  int8_t  alt_deg;             // 0..90
};
struct Sky {
  static constexpr uint8_t kMax = 5;
  uint8_t n;
  SkyObj  obj[kMax];
  uint32_t fresh_ms() const { return 30u * 60u * 1000u; }
};

// observatory/aurora — NOAA planetary Kp, stored ×10.
struct Aurora {
  uint8_t kp_now_x10;
  uint8_t kp_max24_x10;        // forecast max over the next 24 h
  uint32_t fresh_ms() const { return 6u * 3600u * 1000u; }
};

// observatory/gw — most recent significant gravitational-wave event.
enum class GwType : uint8_t { BBH = 0, BNS = 1, NSBH = 2, OTHER = 3 };
struct Gw {
  char     id[12];             // "S251117dq"
  GwType   type;
  uint16_t dist_mly;           // mean luminosity distance, million ly
  int32_t  days_ago;
  uint16_t count;              // significant public events this run
  uint32_t fresh_ms() const { return 48u * 3600u * 1000u; }
};

// observatory/artemis — Moon-program tracker.
struct Artemis {
  static constexpr uint8_t kNameCap = 14;
  static constexpr uint8_t kDescCap = 121;
  char    last[kNameCap];
  int32_t last_local_epoch;
  int8_t  last_result;         // 1 success, 0 failure, 2 partial
  char    next[kNameCap];
  int32_t next_local_epoch;
  bool    next_estimate;
  char    desc[kDescCap];
  uint32_t fresh_ms() const { return 48u * 3600u * 1000u; }
};

// observatory/alert — "go look now" callout. Fresh for duration_s.
enum class AlertKind : uint8_t { ISS = 0, METEOR = 1, PLANET = 2, AURORA = 3, OTHER = 4 };
struct Alert {
  static constexpr uint8_t kLineCap = 14;
  AlertKind kind;
  char      title[kLineCap];
  char      line2[kLineCap];
  char      line3[kLineCap];
  uint16_t  duration_s;
  uint32_t fresh_ms() const { return static_cast<uint32_t>(duration_s) * 1000u; }
};

void init();

// Writers (Core 0). Stamp now_ms as the freshness anchor.
void set(const Weather& v, uint32_t now_ms);
void set(const Sky& v,     uint32_t now_ms);
void set(const Aurora& v,  uint32_t now_ms);
void set(const Gw& v,      uint32_t now_ms);
void set(const Artemis& v, uint32_t now_ms);
void set(const Alert& v,   uint32_t now_ms);

// Readers (any core). Return true + copy when fresh. `age_ms` (optional)
// reports how long ago the value was pushed so scenes can project
// countdowns between publishes.
bool get(uint32_t now_ms, Weather* out, uint32_t* age_ms = nullptr);
bool get(uint32_t now_ms, Sky* out,     uint32_t* age_ms = nullptr);
bool get(uint32_t now_ms, Aurora* out,  uint32_t* age_ms = nullptr);
bool get(uint32_t now_ms, Gw* out,      uint32_t* age_ms = nullptr);
bool get(uint32_t now_ms, Artemis* out, uint32_t* age_ms = nullptr);
bool get(uint32_t now_ms, Alert* out,   uint32_t* age_ms = nullptr);

}  // namespace feeds

// Sky background — vertical gradient + sun on an arc.
//
// Thin wrapper around sky_bg_render::draw() that pulls the current
// epoch from the RTC and converts to UTC. All gradient/sun math
// lives in sky_bg_render.h so SkyTimelapseScene can reuse it with a
// fake epoch.
//
// (added in phase 6.5+ polish)

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "backgrounds/sky_bg_render.h"
#include "config.h"
#include "time_of_day.h"

class SkyBg {
public:
  void init() {}

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    const tod::Reading r = tod::now(now_ms);
    int32_t utc_epoch;
    if (r.valid) {
      utc_epoch = r.local_epoch
                - static_cast<int32_t>(LOCAL_TZ_OFFSET_MIN) * 60;
    } else {
      // Pre-RTC-sync: fall back to a recognisable daytime gradient.
      utc_epoch = 1746360000;
    }
    sky_bg_render::draw(matrix, utc_epoch, LATITUDE_DEG, LONGITUDE_DEG);
  }
};

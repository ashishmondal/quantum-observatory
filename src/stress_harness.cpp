#include "stress_harness.h"

#if defined(CORE0_STRESS) || defined(CORE0_MQTT_FLOOD)
#include <Arduino.h>
#include <string.h>
#endif

#ifdef CORE0_MQTT_FLOOD
#include "scene_state.h"
#endif

namespace stress_harness {

void tick(uint32_t now_ms) {
#ifdef CORE0_STRESS
  // CPU-bound JSON-parse + checksum. Buffer sized to the documented
  // Scene Contract budget (NFR-2.3: 512 B + headroom). Static
  // allocation, no heap, no float (NFR-2.2 / NFR-1.3).
  {
    static uint8_t  s_stress_buf[512];
    static uint32_t s_stress_iters       = 0;
    static uint32_t s_stress_last_log_ms = 0;
    static const char kPayload[] =
        "{\"scene_id\":\"jupiter_visibility\",\"priority\":3,"
        "\"duration\":30,\"sticky\":false,"
        "\"overrides\":{\"text\":\"Visible: East @ 9PM\",\"val\":\"78\"}}";
    constexpr size_t kPayloadLen = sizeof(kPayload) - 1;
    for (int i = 0; i < 200; ++i) {
      memcpy(s_stress_buf, kPayload,
             kPayloadLen < sizeof(s_stress_buf) ? kPayloadLen
                                                : sizeof(s_stress_buf));
      uint32_t sum = 0;
      for (size_t j = 0; j < sizeof(s_stress_buf); ++j) {
        sum = sum * 31u + s_stress_buf[j];
      }
      s_stress_iters += sum;
    }
    if (now_ms - s_stress_last_log_ms >= 1000u) {
      s_stress_last_log_ms = now_ms;
      Serial.print("[stress] core0 iters=");
      Serial.println(static_cast<unsigned long>(s_stress_iters));
    }
  }
#endif

#ifdef CORE0_MQTT_FLOOD
  {
    static uint32_t s_flood_last_ms = 0;
    static uint32_t s_flood_count   = 0;
    static uint32_t s_flood_log_ms  = 0;
    static bool     s_flood_toggle  = false;
    if (now_ms - s_flood_last_ms >= 50u) {  // 20 Hz
      s_flood_last_ms = now_ms;
      const auto id = s_flood_toggle ? scene_state::SceneId::CLOCK
                                     : scene_state::SceneId::BG_NEBULA;
      s_flood_toggle = !s_flood_toggle;
      scene_state::request(id, 1, 30, false);
      ++s_flood_count;
    }
    if (now_ms - s_flood_log_ms >= 1000u) {
      s_flood_log_ms = now_ms;
      Serial.print("[flood] requests=");
      Serial.println(static_cast<unsigned long>(s_flood_count));
    }
  }
#endif

#if !defined(CORE0_STRESS) && !defined(CORE0_MQTT_FLOOD)
  (void)now_ms;
#endif
}

}  // namespace stress_harness

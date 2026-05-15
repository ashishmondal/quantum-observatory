#include "rtc_poll.h"

#include <Arduino.h>

#include "time_of_day.h"

namespace rtc_poll {

void tick(uint32_t now_ms) {
  // RTC poll cadence:
  //   - Default: every 1 hour. DS3231 drift is ~2 ppm (≈7 s/month),
  //     so the millis() projection in tod::now() is more than
  //     accurate enough between hourly resyncs.
  //   - Glitch handling: poll_validated() rejects readings that
  //     differ from the projected wall-clock by more than 3 hours
  //     (covers DST jumps, MQTT-driven set_from_mqtt corrections,
  //     and outright corruption). On reject we re-try with
  //     exponential backoff: 1 s → 2 s → 4 s → ... → 1 h, resetting
  //     to the 1 h cadence on the first accept.
  //   - First poll: scheduled immediately so chrome leaves "--:--"
  //     ASAP after boot.
  static uint32_t s_next_poll_at_ms = 0;
  static uint32_t s_backoff_ms      = 1000u;
  static constexpr uint32_t kPollOk_ms  = 60u * 60u * 1000u;  // 1 h
  static constexpr uint32_t kBackoffCap = kPollOk_ms;
  static constexpr uint32_t kMaxJumpSec = 3u * 60u * 60u;     // 3 h
  if (static_cast<int32_t>(now_ms - s_next_poll_at_ms) >= 0) {
    const bool accepted = tod::poll_validated(now_ms, kMaxJumpSec);
    if (accepted) {
      s_next_poll_at_ms = now_ms + kPollOk_ms;
      s_backoff_ms      = 1000u;
    } else {
      s_next_poll_at_ms = now_ms + s_backoff_ms;
      s_backoff_ms      = (s_backoff_ms >= kBackoffCap / 2u)
                            ? kBackoffCap
                            : (s_backoff_ms * 2u);
      Serial.print("[time] poll rejected, retry in ms=");
      Serial.println(static_cast<unsigned long>(s_backoff_ms));
    }
  }
}

}  // namespace rtc_poll

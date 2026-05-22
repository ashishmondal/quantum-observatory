#include "launch_imminent.h"

#include <Arduino.h>

#include "buzzer.h"
#include "launch_state.h"
#include "light_sensor.h"
#include "scene_state.h"
#include "time_of_day.h"

namespace launch_imminent {

namespace {

// "Heads-up" alert — 4-note rising arpeggio distinct from the ISS
// "ta-da" (G5/B5/E6) and the T-0 ignition sting (C5/E5/G5 held). We
// climb C6 → E6 → G6 → C7 in quick succession (80/80/80/200 ms =
// 440 ms total, well under the FR-10.7 1500 ms ceiling) so the
// timbre reads as a fanfare for an incoming event, not a chord.
constexpr buzzer::Note kImminentMelody[] = {
    {1047,  80},  // C6
    {1319,  80},  // E6
    {1568,  80},  // G6
    {2093, 200},  // C7
};
constexpr uint8_t kImminentMelodyCount =
    sizeof(kImminentMelody) / sizeof(kImminentMelody[0]);

}  // namespace

void tick(uint32_t now_ms) {
  static uint32_t s_last_check_ms = 0;
  static bool     s_was_imminent  = false;
  static bool     s_initialized   = false;

  // 1 Hz cadence (mirrors iss_visibility::tick). First call always
  // evaluates so a boot snapshot inside the imminent window doesn't
  // sit on an artificial "not imminent" default for a whole second.
  if (s_initialized && (now_ms - s_last_check_ms) < 1000u) return;
  s_last_check_ms = now_ms;

  launch_state::Snapshot ls;
  const bool fresh = launch_state::get(now_ms, &ls);
  const tod::Reading r = tod::now(now_ms);

  bool now_imminent = false;
  int32_t t_minus = 0;
  if (fresh && r.valid) {
    t_minus = ls.t0_local_epoch - r.local_epoch;
    now_imminent = (t_minus >= 0 && t_minus <= kImminentWindowS);
  }
  // FR-10.9 / FR-7.2 night-mode courtesy: even though buzzer::play()
  // is silent at night and the NIGHT compositor overlay would mask
  // the scene visually, an unmuted screen brightness swap and the
  // sticky priority-5 grab could still disturb a sleeping operator
  // on the falling edge of night. Treat night-active as "no
  // imminent" so neither the alert nor the preemption fires; the
  // launch_countdown scene is still reachable by manual IR/MQTT
  // request if someone is awake and looking for it.
  if (light_sensor::is_night()) {
    now_imminent = false;
  }

  // Suppress the very first rising-edge if we boot inside the
  // imminent window — the operator likely already requested the
  // scene manually if they care, and a stale boot ta-da is noise.
  if (!s_initialized) {
    s_was_imminent = now_imminent;
    s_initialized  = true;
    return;
  }

  if (now_imminent && !s_was_imminent) {
    Serial.print("[launch] T-");
    Serial.print(static_cast<long>(t_minus));
    Serial.println("s → imminent: switch + alert");
    // Priority 5 (the FR-2.1 ceiling) sticky so a low-priority
    // Director cycle can't yank us off the countdown before liftoff.
    // user_intent=false so an operator IR/MQTT request (which sets
    // user_intent=true) still wins if they want to look at
    // something else. Sticky stays in force until launch_imminent
    // clears it on the falling edge below.
    scene_state::request(scene_state::SceneId::LAUNCH_COUNTDOWN,
                         /*priority=*/5,
                         /*duration_s=*/0,   // ignored when sticky
                         /*sticky=*/true,
                         /*user_intent=*/false);
    buzzer::play(kImminentMelody, kImminentMelodyCount);
  } else if (!now_imminent && s_was_imminent) {
    Serial.println("[launch] imminent window closed");
    if (scene_state::current() == scene_state::SceneId::LAUNCH_COUNTDOWN) {
      scene_state::clear_sticky();
    }
  }

  s_was_imminent = now_imminent;
}

}  // namespace launch_imminent

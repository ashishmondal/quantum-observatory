#include "iss_visibility.h"

#include <Arduino.h>

#include "buzzer.h"
#include "config.h"
#include "iss_geometry.h"
#include "iss_state.h"
#include "scene_state.h"
#include "sun_position.h"
#include "time_of_day.h"

namespace iss_visibility {

namespace {

// "Ta-da!" — G5 → B5 → E6 ascending major triad in the C5..C6 sweet
// spot of the carrier piezo so the rising shape is actually parsed
// as triumphant rather than as "another shrill beep". Distinct from
// any theme melody so the operator can tell ISS-rise from
// theme-change by ear. Total = 80+40+240 = 360 ms (well under the
// 1500 ms FR-10.7 cap).
constexpr buzzer::Note kIssVisibleMelody[] = {
    { 784,  80},   // G5  — "ta"
    { 988,  40},   // B5  — (lift)
    {1319, 240},   // E6  — "da!" held
};
constexpr uint8_t kIssVisibleMelodyCount =
    sizeof(kIssVisibleMelody) / sizeof(kIssVisibleMelody[0]);

}  // namespace

bool is_visible_now(uint32_t now_ms) {
  iss_state::Snapshot iss;
  if (!iss_state::get(now_ms, &iss)) return false;
  if (!iss.sunlit) return false;

  const tod::Reading r = tod::now(now_ms);
  if (!r.valid) return false;

  const iss_geom::LookAngles la = iss_geom::look_angles(
      LATITUDE_DEG, LONGITUDE_DEG,
      iss.iss_lat_deg, iss.iss_lon_deg,
      static_cast<float>(iss.altitude_km));
  if (la.elevation_deg < 0.0f) return false;

  const int32_t utc_epoch = r.local_epoch
      - static_cast<int32_t>(LOCAL_TZ_OFFSET_MIN) * 60;
  const sun::Position sp =
      sun::compute(utc_epoch, LATITUDE_DEG, LONGITUDE_DEG);
  return sp.altitude_deg <= -6.0f;
}

void tick(uint32_t now_ms) {
  static uint32_t s_last_check_ms = 0;
  static bool     s_was_visible   = false;
  static bool     s_initialized   = false;

  // 1 Hz cadence; first call always evaluates so the boot snapshot
  // doesn't sit on an artificial "not visible" assumption.
  if (s_initialized && (now_ms - s_last_check_ms) < 1000u) return;
  s_last_check_ms = now_ms;

  const bool now_visible = is_visible_now(now_ms);

  // Suppress the rising edge on the very first evaluation: if the
  // panel boots into an in-progress pass we don't want a stale
  // ta-da on second 1. The user can navigate to the ISS scene
  // manually, and the next pass's true rising edge will fire.
  if (!s_initialized) {
    s_was_visible = now_visible;
    s_initialized = true;
    return;
  }

  if (now_visible && !s_was_visible) {
    // Rising edge — fire the auto-switch + ta-da. Sticky at
    // priority 4 so a low-priority Director cycle can't yank us
    // out, but user_intent=true on IR/MQTT requests defeats the
    // sticky lock if the operator wants to look at something else.
    Serial.println("[iss] visibility rising edge → auto-switch + ta-da");
    scene_state::request(scene_state::SceneId::ISS_PASS,
                         /*priority=*/4,
                         /*duration_s=*/0,   // ignored when sticky
                         /*sticky=*/true,
                         /*user_intent=*/false);  // firmware-initiated
    buzzer::play(kIssVisibleMelody, kIssVisibleMelodyCount);
  } else if (!now_visible && s_was_visible) {
    // Falling edge — pass ended. If we're still on the auto-switched
    // ISS scene clear the sticky so the default CLOCK takes over
    // after the standard fade. If the operator navigated away
    // during the pass, leave their chosen scene alone.
    Serial.println("[iss] visibility falling edge");
    if (scene_state::current() == scene_state::SceneId::ISS_PASS) {
      scene_state::clear_sticky();
    }
  }

  s_was_visible = now_visible;
}

}  // namespace iss_visibility

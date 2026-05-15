// Shared 3-line typewriter timing kernel — used by IssPassScene,
// MoonPhaseScene, JupiterVisibilityScene, ConstellationNowScene to
// derive (typed glyphs per line, currently-typing line, cursor blink)
// from now_ms + line lengths. Render and styling stay scene-local;
// only the schedule arithmetic is shared.
//
// Cycle: line 1 types out → kPauseMs → line 2 types out → kPauseMs →
// line 3 types out → kPauseMs → kHoldMs → loop. Caret is on/off in
// 280 ms half-periods.

#pragma once

#include <stdint.h>

namespace typewriter {

struct Schedule {
  uint8_t typed[3];   // 0..lens[i] — glyphs revealed this frame
  int8_t  active;     // 0/1/2 = line currently typing; -1 = holding
  bool    cursor_on;  // caret blink state (50 % duty, 280 ms half)
};

constexpr uint32_t kCharMs  = 90;    // per-glyph type rate (FR-9 typewriter)
constexpr uint32_t kPauseMs = 600;   // pause after a line completes
constexpr uint32_t kHoldMs  = 2500;  // hold after all 3 done before restart
constexpr uint32_t kBlinkMs = 280;   // caret half-period

inline Schedule compute(uint32_t now_ms, const uint8_t lens[3]) {
  const uint32_t span1 = lens[0] * kCharMs + kPauseMs;
  const uint32_t span2 = lens[1] * kCharMs + kPauseMs;
  const uint32_t span3 = lens[2] * kCharMs + kPauseMs;
  const uint32_t total = span1 + span2 + span3 + kHoldMs;
  const uint32_t t     = now_ms % total;
  const uint32_t starts[3] = { 0u, span1, span1 + span2 };

  Schedule s{};
  s.active = -1;
  for (int i = 0; i < 3; ++i) {
    if (t < starts[i]) {
      s.typed[i] = 0;
    } else {
      const uint32_t local      = t - starts[i];
      const uint32_t typing_dur = lens[i] * kCharMs;
      if (local < typing_dur) {
        s.typed[i] = static_cast<uint8_t>(local / kCharMs);
        s.active   = static_cast<int8_t>(i);
      } else {
        s.typed[i] = lens[i];
      }
    }
  }
  s.cursor_on = ((now_ms / kBlinkMs) & 1u) == 0u;
  return s;
}

}  // namespace typewriter

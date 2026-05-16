// Dev-only manual trigger for the giant clock digit-roll animation.
//
// Lets the operator fire a synthetic minute / ten-min / hour cascade
// without waiting for the real RTC tick. Exercise paths:
//   - IR remote (CLOCK scene only): LEFT=minute, RIGHT=ten-min,
//     OK=hour. Gated behind -DCLOCK_ANIM_TEST so the production
//     build keeps LEFT/RIGHT on theme::cycle and OK on
//     info_overlay_toggle.
//   - MQTT topic `observatory/test/clock_anim` with payload
//     `{"kind":"minute"|"ten_min"|"hour"}`. Same -DCLOCK_ANIM_TEST
//     gate.
//
// Cross-core wiring follows the same pattern as
// g_info_overlay_event_ms (CODING_PRACTICES §3): a single naturally-
// aligned uint8_t. Single Core-0 writer (IR action / MQTT handler),
// single Core-1 reader (GiantClockScene::render()), edge-detected
// and reset to 0 on consume — sentinel 0 = "no event pending".

#pragma once

#include <stdint.h>

namespace clock_anim_test {

enum class Kind : uint8_t {
  NONE    = 0,
  MINUTE  = 1,   // M2 only — 300 ms
  TEN_MIN = 2,   // M1 + M2 — 300 / 600 ms
  HOUR    = 3,   // H2 + M1 + M2 — 300 / 600 / 900 ms
};

}  // namespace clock_anim_test

// Naturally-aligned uint8 — atomic on RP2040, no mutex needed.
// Defined in src/diag/clock_anim_test.cpp (one TU owns the storage).
extern volatile uint8_t g_clock_anim_test_kind;

// Cross-core click signal for the giant clock's digit-roll
// animation "stepper" sound. Core 1 (GiantClockScene) writes a new
// packed value once per visible digit step; Core 0 (main loop)
// edge-detects the change and fires buzzer::tick_click() with the
// per-slot pitch.
//
// Packing: low 3 bits = digit slot index (0..4, matches kSlotX);
// upper 29 bits = monotonic counter that increments on every step.
// The whole word is read/written atomically (naturally-aligned
// uint32 on RP2040, CODING_PRACTICES §3), so Core 0 always sees a
// consistent (counter, slot) pair without a mutex.
extern volatile uint32_t g_clock_anim_click_seq;

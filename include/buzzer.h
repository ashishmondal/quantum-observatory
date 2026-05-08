// Piezo buzzer driver — short non-blocking chirps for IR-remote feedback.
//
// Hardware: GP27 on the Waveshare Pico-RGB-Matrix carrier (active-high,
// see HARDWARE.md "Buzzer (GP27, active-high)"). Treated as a passive
// piezo driven via Arduino tone(): the arduino-pico core implements
// tone(pin, freq, duration_ms) on top of a hardware PWM slice + a
// timer-fired stop, so the call returns immediately and silence
// arrives ~`duration_ms` later without blocking the loop. Works
// equally well on an active buzzer (the carrier internal oscillator
// just rectifies our PWM into its fixed pitch) — we lose tone control
// but the on/off envelope is still right.
//
// Project policy (FR-10.5): every tone the firmware produces stays
// >= 8 kHz. The floor is enforced inside play() / chirp() so a future
// caller can't accidentally drop below it. Tones < 8 kHz are dropped
// (treated as a rest of the same duration).
//
// Bound + called on Core 0 only — no cross-core safety. Callers:
//   - ir_remote::poll()  — single-tone chirp on accepted press (FR-10.6).
//   - theme::set()       — per-theme signature melody on rising-edge
//                          theme change (FR-10.7, wired in B.3).

#pragma once

#include <stdint.h>

namespace buzzer {

// One note in a flash-resident melody. `freq_hz == 0` is a rest
// (silent gap of `ms`). Both fields are uint16_t so a `Note[]` is
// 4 bytes per entry — a 4-note melody costs 16 bytes of flash.
struct Note {
  uint16_t freq_hz;   // tone pitch (>= 8 kHz, else dropped to a rest)
  uint16_t ms;        // note duration; also used as the rest duration when freq_hz == 0
};

// One-time bring-up. Idempotent. Sets the pin LOW (silent). Fires
// the FR-10.4 boot self-test (single short tone) so the wiring is
// audibly confirmed without being annoying.
void begin();

// Short feedback chirp for "the remote did something" (FR-10.6).
// ~12 ms at ~9 kHz — short enough to feel instantaneous, high
// enough to sit above the 8 kHz floor. Non-blocking. Cancels any
// in-flight melody (the user just pressed a key — a fresh ack
// beats a stale theme cue).
void chirp();

// Start playing a flash-resident note sequence (FR-10.7). `notes`
// MUST outlive the playback (use a file-scope `static constexpr
// Note[]`). `n` is silently clamped to kMaxMelodyNotes. Cancels
// any in-flight melody by calling noTone() first, so rapid
// back-to-back play() calls don't overlap — the latest cue wins.
// Non-blocking: tick() drives note advancement.
//
// Notes whose freq_hz is below the 8 kHz floor are emitted as
// rests of the same duration (FR-10.5 enforcement at the driver
// boundary, not at the call site).
void play(const Note* notes, uint8_t n);

// Drive the playback state machine. Call from Core 0 loop() every
// iteration. Cheap when nothing is playing (one millis() compare).
// Safe to call before begin() (no-ops).
void tick(uint32_t now_ms);

// Maximum melody length the scheduler will honour. The five v1
// theme melodies (FR-10.7 table) top out at 4 notes; 8 leaves
// headroom without committing to a runaway-long sequence.
constexpr uint8_t kMaxMelodyNotes = 8;

}  // namespace buzzer

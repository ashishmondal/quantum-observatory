// Piezo buzzer driver — see buzzer.h for the full rationale.

#include <Arduino.h>

#include "buzzer.h"
#include "config.h"

namespace buzzer {

namespace {

bool s_begun = false;

// Single-tone feedback chirp pitch (FR-10.6 key-press tick + FR-10.4
// boot self-test). 9 kHz sits well above conversational pitch and
// the dominant peaks of typical TV / music content, so the chirp
// reads as a "device tick" rather than competing with foreground
// audio in the room. It's also past the typical 4 kHz resonant peak
// of the carrier piezo so the response is intentionally quieter
// than max — exactly what a key-press tick should be. (No project-
// wide pitch floor any more — see buzzer.h policy block.)
constexpr uint16_t kChirpFreqHz   = 9000;
// 12 ms ≈ one-and-a-bit cycles at the 108 ms NEC frame interval —
// short enough that even a fast finger-tap doesn't run two chirps
// into each other, long enough that the piezo's mechanical envelope
// can actually start producing sound before we cut it.
constexpr uint16_t kChirpMs       = 12;
// FR-10.4 boot self-test: a single short tone at the chirp pitch
// fired once from begin(). 30 ms <= 50 ms cap; long enough to be
// audible across the room, short enough to not be annoying.
constexpr uint16_t kSelfTestMs    = 30;

// ---- Non-blocking melody scheduler (FR-10.7 plumbing) ---------------------
//
// Single-shot state machine: when play() is called, we copy a tiny
// header (pointer + count) and arm note 0; tick() walks the cursor
// on millis() deadlines, calling tone(pin, hz, ms) at each note
// boundary so the actual on/off envelope is hardware-timer-driven
// (the PWM slice + a one-shot timer, see arduino-pico's tone()).
// We never call delay(); tick() is just one millis() compare in
// the steady "still playing the current note" path.
//
// Concurrency: Core 0 only — both writer (play / chirp) and reader
// (tick) live on the same core, so no atomics or mutex needed. The
// writer-cancel-then-arm sequence (`noTone()` → store new pointer →
// reset cursor) is naturally serialised by the single-thread call
// site.

const Note* s_seq         = nullptr;   // active sequence; nullptr ⇒ idle
uint8_t     s_seq_len     = 0;         // total notes in s_seq
uint8_t     s_seq_idx     = 0;         // index of the currently-sounding note
uint32_t    s_note_end_ms = 0;         // millis() deadline for the current note

// Stop any in-flight sound, clear the sequence pointer. Used by
// play() before arming a new sequence and by tick() at end-of-song.
inline void stop_internal() {
  noTone(PIN_BUZZER);
  digitalWrite(PIN_BUZZER, LOW);   // tone() leaves the pin in PWM mode otherwise
  s_seq         = nullptr;
  s_seq_len     = 0;
  s_seq_idx     = 0;
  s_note_end_ms = 0;
}

// Start sounding s_seq[s_seq_idx]. Honours the FR-10.5 floor: notes
// below kMinFreqHz are emitted as silence of the same duration so
// the timing of the rest of the melody is preserved (also catches
// freq_hz == 0, the explicit-rest convention).
inline void start_current_note(uint32_t now_ms) {
  const Note& n = s_seq[s_seq_idx];
  const uint16_t dur_ms = n.ms == 0 ? 1 : n.ms;  // guard div-by-zero in deadline math
  if (n.freq_hz != 0) {
    // No floor here on purpose — see kMinChirpHz comment above.
    // 0 is the only value that means "rest".
    tone(PIN_BUZZER, n.freq_hz, dur_ms);
  } else {
    noTone(PIN_BUZZER);              // explicit silence (freq_hz == 0 sentinel)
    digitalWrite(PIN_BUZZER, LOW);
  }
  s_note_end_ms = now_ms + dur_ms;
}

}  // namespace

void begin() {
  if (s_begun) return;
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);   // ensure silent at boot
  s_begun = true;
  Serial.print("[buzzer] begin pin=");
  Serial.print(static_cast<int>(PIN_BUZZER));
  Serial.print(" self-test ");
  Serial.print(static_cast<int>(kChirpFreqHz));
  Serial.print("Hz/");
  Serial.print(static_cast<int>(kSelfTestMs));
  Serial.println("ms");
  // FR-10.4 self-test. Non-blocking; returns in microseconds. Not
  // routed through play() because it's a single tone with no state
  // machine cost — and we want it to fire even before the first
  // tick() (which won't happen until loop() starts).
  tone(PIN_BUZZER, kChirpFreqHz, kSelfTestMs);
}

void chirp() {
  if (!s_begun) return;
  // A user-input ack (FR-10.6) MUST cut through any in-flight
  // melody — the user just pressed a key and the visual response
  // is already happening; a stale theme cue from 200 ms ago must
  // not delay the audible "got it".
  stop_internal();
  // arduino-pico's tone() with a duration arg is non-blocking: it
  // configures the PWM slice and schedules the stop on a hardware
  // timer, so this returns in microseconds.
  tone(PIN_BUZZER, kChirpFreqHz, kChirpMs);
}

void play(const Note* notes, uint8_t n) {
  if (!s_begun) return;
  if (notes == nullptr || n == 0) return;
  if (n > kMaxMelodyNotes) n = kMaxMelodyNotes;
  // Rapid back-to-back play() (e.g. operator spam-cycling themes
  // on the IR remote) — the latest cue wins. stop_internal() also
  // clears any prior cursor state so we don't leak old indices.
  stop_internal();
  s_seq         = notes;
  s_seq_len     = n;
  s_seq_idx     = 0;
  start_current_note(millis());
}

void tick(uint32_t now_ms) {
  // Hot-path early-out: nothing playing. One compare, one branch.
  // Also covers the not-yet-begun case (s_seq is nullptr).
  if (s_seq == nullptr) return;
  // Wrap-safe deadline check: handle 49-day millis() rollover via
  // signed subtraction (CODING_PRACTICES §2 time-math rule). Note
  // durations are always << INT32_MAX so there's no risk of the
  // delta itself overflowing.
  if (static_cast<int32_t>(now_ms - s_note_end_ms) < 0) return;
  // Current note done. Advance, or stop at end-of-song.
  ++s_seq_idx;
  if (s_seq_idx >= s_seq_len) {
    stop_internal();
    return;
  }
  start_current_note(now_ms);
}

}  // namespace buzzer

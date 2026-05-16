// Piezo buzzer driver — see buzzer.h for the full rationale.

#include <Arduino.h>

#include "buzzer.h"
#include "config.h"

namespace buzzer {

namespace {

bool s_begun = false;

// FR-10.8 / FR-10.9 driver-boundary mute. Defaults to true so any
// cue fired before main.cpp computes the first (splash || night)
// snapshot is silently dropped — the most common offender is the
// FR-10.7 theme melody triggered when prefs::begin() restores the
// last-saved theme during setup(), well before the splash overlay
// could possibly clear. main.cpp lifts this on the falling edge of
// (splash || night) once the renderer is live.
bool s_quiet = true;

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
// (FR-10.4 boot self-test removed by FR-10.8 boot quiet \u2014 the
// kSelfTestMs constant was its only user.)

// Digit-roll "tick" — short envelope (6 ms) so a 30 ms-spaced
// burst during a cascade reads as a mechanical stepper. Pitch is
// caller-supplied so each digit slot (H2 / M1 / M2) gets its own
// voice; the buzzer driver just enforces the envelope and the
// re-arm coalescing window.
constexpr uint16_t kClickMs       = 6;

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
  Serial.println(" quiet=1 (FR-10.8 boot)");
  // FR-10.8 supersedes the prior FR-10.4 boot self-test chirp \u2014
  // the buzzer is unconditionally silent during the boot phase.
  // The wiring witness is now the first post-boot cue (an IR-press
  // chirp or the next theme melody fired after main.cpp lifts the
  // quiet gate on splash-clear).
}

void chirp() {
  if (!s_begun) return;
  if (s_quiet) return;   // FR-10.8 / FR-10.9 — silent during boot or night.
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

void tick_click(uint16_t freq_hz) {
  if (!s_begun) return;
  if (s_quiet) return;   // FR-10.8 / FR-10.9 — silent during boot or night.
  if (freq_hz == 0) return;   // 0 is the rest sentinel in Note[]
  // Coalesce rapid clicks: if the previous click is still ringing
  // (within kClickMs of the last call), skip re-arming the PWM.
  // Without this, two clicks closer than ~6 ms cancel each other
  // out (the tone() restart truncates the first click before the
  // ear can register it).
  static uint32_t s_last_click_ms = 0;
  const uint32_t now = millis();
  if (now - s_last_click_ms < kClickMs) return;
  s_last_click_ms = now;
  // No stop_internal() — if a melody is playing (e.g. theme switch
  // mid-cascade), let it continue. The clicks are decorative and
  // shouldn't trample a deliberate audio cue.
  if (s_seq != nullptr) return;
  tone(PIN_BUZZER, freq_hz, kClickMs);
}

void play(const Note* notes, uint8_t n) {
  if (!s_begun) return;
  if (s_quiet) return;   // FR-10.8 / FR-10.9 — silent during boot or night.
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

void set_quiet(bool quiet) {
  // Idempotent. On the rising edge (silence engaging), stop any
  // in-flight melody immediately so a long tail (e.g. the 700 ms
  // Goldsmith dissonance at the end of nostromo_green's wakeup)
  // doesn't continue ringing into the night-mode swap. On the
  // falling edge (silence lifting), do nothing — there's no queued
  // playback per FR-10.8 / FR-10.9 (cues issued during quiet are
  // dropped at the entry points, not deferred).
  if (quiet == s_quiet) return;
  s_quiet = quiet;
  if (quiet) stop_internal();
}

bool is_quiet() { return s_quiet; }

}  // namespace buzzer

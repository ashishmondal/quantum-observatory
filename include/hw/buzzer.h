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
// Tone-pitch policy: there is no driver-side floor. Each call site
// picks a pitch that suits the cue — feedback chirps stay high
// (~9 kHz) so a key-press tick still reads as a tick over ambient
// audio; signature melodies (FR-10.7) and the boot melody (FR-10.4)
// use the C4..C7 musical octaves where the carrier piezo's 4 kHz
// resonant envelope can faithfully reproduce pitch and the human
// ear actually parses the motif as a tune. The only forbidden
// frequency in `Note::freq_hz` is 0, reserved as the rest sentinel.
//
// Bound + called on Core 0 only — no cross-core safety. Callers:
//   - ir_remote::poll()  — single-tone chirp on accepted press (FR-10.6).
//   - theme::set()       — per-theme signature melody on rising-edge
//                          theme change (FR-10.7, wired in B.3).
//   - setup()            — Westminster boot melody (FR-10.4 audible
//                          "device awake" cue).

#pragma once

#include <stdint.h>

namespace buzzer {

// One note in a flash-resident melody. `freq_hz == 0` is a rest
// (silent gap of `ms`). Both fields are uint16_t so a `Note[]` is
// 4 bytes per entry — a 4-note melody costs 16 bytes of flash.
struct Note {
  uint16_t freq_hz;   // tone pitch in Hz; 0 ⇒ rest. No driver-side
                      // floor (see policy block above). Useful range
                      // on the carrier piezo is roughly 200 Hz ..
                      // 16 kHz; below ~200 Hz the mechanical envelope
                      // barely registers.
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

// Tiny mechanical "tick" for the giant clock's digit-roll
// animation. ~6 ms at the caller-supplied pitch. Each digit slot
// (H2 / M1 / M2) uses a different `freq_hz` so a cascade reads as
// distinct stepper voices rather than a single uniform stutter.
//
// Volume policy: the carrier piezo peaks near 4 kHz; the further
// `freq_hz` sits from that resonance, the quieter the click. Typical
// callers use 300–600 Hz (deep in the mechanical-response falloff
// tail, ~3 octaves below resonance) so the stepper is a soft tick
// rather than a sharp ack. There's no software duty-cycle lever
// (arduino-pico's tone() is fixed 50 %), so pitch placement is the
// only volume control. Below ~200 Hz the piezo barely moves air.
//
// Non-blocking. Does NOT cancel an in-flight melody (theme cue
// during a cascade wins). Coalesces re-arms within the click
// duration so adjacent slots' clicks don't truncate each other
// (see buzzer.cpp).
void tick_click(uint16_t freq_hz);

// Start playing a flash-resident note sequence (FR-10.7 / FR-10.4
// boot melody). `notes` MUST outlive the playback (use a file-scope
// `static constexpr Note[]`). `n` is silently clamped to
// kMaxMelodyNotes. Cancels any in-flight melody by calling noTone()
// first, so rapid back-to-back play() calls don't overlap — the
// latest cue wins. Non-blocking: tick() drives note advancement.
//
// `freq_hz == 0` is honoured as a silent rest of the requested
// duration; all other frequencies pass straight through to tone().
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

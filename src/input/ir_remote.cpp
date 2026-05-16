// IR remote receiver — POC implementation.
//
// IMPORTANT: IRremote v4 ships its implementation inside `IRremote.hpp`
// (header-only-with-impl), so this header MUST be included from exactly
// one .cpp in the project. Other modules consume `ir_remote.h` only.
//
// Bound on Core 0; see ir_remote.h for the full rationale.

#include <Arduino.h>

#include "buzzer.h"
#include "config.h"
#include "ir_remote.h"
#include "prefs.h"

// Disable the library's debug Serial chatter (one line per frame on
// the global Serial port — would race Core 1's Protomatter timing if
// we ever moved this to Core 1, and is just noise on Core 0).
#define NO_LED_FEEDBACK_CODE
#include <IRremote.hpp>

namespace ir_remote {

namespace {

Stats s_stats{};
bool  s_begun = false;

// FR-17.5 dispatch table — installed by main.cpp::setup() via
// set_dispatch(). Pointer + count + expected-address are file-static
// so poll() can read them lock-free (Core 0 is the only writer AND
// reader; no cross-core access).
const DispatchEntry* s_dispatch_table   = nullptr;
uint8_t              s_dispatch_count   = 0;
uint16_t             s_expected_address = 0;
bool                 s_dispatch_enabled = true;

}  // namespace

void begin() {
  if (s_begun) return;
  // Second arg = ENABLE_LED_FEEDBACK (we have no convenient feedback
  // LED — the panel's LEDs are owned by Core 1 / Protomatter, the
  // buzzer would be obnoxious). False keeps the ISR minimal.
  IrReceiver.begin(PIN_IR_RX, /*enableLEDFeedback=*/false);
  s_begun = true;
  Serial.print("[ir] begin pin=");
  Serial.print(static_cast<int>(PIN_IR_RX));
  Serial.println(" lib=IRremote-v4");
}

bool poll() {
  if (!s_begun) return false;
  bool any = false;
  // Drain in case multiple frames queued between polls (a long-press
  // remote can emit a frame every ~108 ms; loop() runs every ~10 ms,
  // so normally at most one frame is waiting — but be defensive).
  while (IrReceiver.decode()) {
    any = true;
    const auto& d = IrReceiver.decodedIRData;
    ++s_stats.decoded;
    if (d.flags & IRDATA_FLAGS_IS_REPEAT)      ++s_stats.repeats;
    if (d.flags & IRDATA_FLAGS_PARITY_FAILED)  ++s_stats.parity_failed;
    if (d.protocol == UNKNOWN)                 ++s_stats.unknown;
    // IRDATA_FLAGS_WAS_OVERFLOW signals raw-buffer overrun (host loop
    // too slow). Per the library API, a true here means the decode is
    // bogus — count it but don't trust the protocol/cmd fields.
    if (d.flags & IRDATA_FLAGS_WAS_OVERFLOW)   ++s_stats.overflows;

    s_stats.last.protocol  = static_cast<uint8_t>(d.protocol);
    s_stats.last.address   = d.address;
    s_stats.last.command   = d.command;
    s_stats.last.raw_data  = d.decodedRawData;
    s_stats.last.num_bits  = d.numberOfBits;
    s_stats.last.flags     = d.flags;
    s_stats.last.at_ms     = millis();

    // ---- FR-17.2 / FR-17.3 / FR-17.5 filter + dispatch ---------------
    //
    // Order matters: protocol/parity/overflow rejects come first
    // (they're decided regardless of who's listening), then the
    // address gate (a "this isn't my remote" signal), then the
    // dispatch lookup (which can be disabled in-flight by the IR.2
    // learning wizard via set_dispatch_enabled(false)).
    const bool is_nec       = (d.protocol == NEC);
    const bool parity_bad   = (d.flags & IRDATA_FLAGS_PARITY_FAILED) != 0;
    const bool overflowed   = (d.flags & IRDATA_FLAGS_WAS_OVERFLOW)   != 0;
    const bool is_repeat    = (d.flags & IRDATA_FLAGS_IS_REPEAT)      != 0;

    do {
      if (!is_nec || parity_bad || overflowed) break;  // already counted above
      if (d.address != s_expected_address) {
        ++s_stats.addr_rejected;
        break;
      }
      if (!s_dispatch_enabled || s_dispatch_table == nullptr) break;

      bool matched = false;
      for (uint8_t i = 0; i < s_dispatch_count; ++i) {
        const DispatchEntry& e = s_dispatch_table[i];
        if (e.command != d.command) continue;
        matched = true;
        // FR-17.4: discrete actions ignore repeats so a long-press
        // doesn't stampede. Continuous actions (none in v1) opt in.
        if (is_repeat && !e.honour_repeats) break;
        // Audible feedback: chirp BEFORE the action runs (FR-10.6).
        // Order matters — buzzer::chirp() and buzzer::play() share
        // one scheduler, and any new sound cancels the in-flight
        // one (latest-wins). If the chirp came AFTER the action, an
        // action that itself plays a melody (theme::cycle → set →
        // buzzer::play, FR-10.7 / B.3) would have its melody
        // immediately killed by the chirp. Putting the chirp first
        // means non-melody actions (scene cycle, overlay toggle)
        // still get the tick, while melody actions cleanly replace
        // the brief chirp with their longer cue.
        //
        // FR-19 settings overlay adds a per-user opt-out: when the
        // operator has muted button feedback, drop the chirp here.
        // The boot/night quiet gates inside buzzer:: still take
        // precedence (FR-10.8 / FR-10.9) — prefs gate is in
        // addition, not in place of.
        if (prefs::current().button_sound) {
          buzzer::chirp();
        }
        if (e.action != nullptr) {
          e.action(d.address, d.command);
        }
        ++s_stats.accepted;
        break;
      }
      if (!matched) ++s_stats.unmapped;
    } while (false);

    IrReceiver.resume();   // arm for the next frame
  }
  return any;
}

Stats stats() {
  return s_stats;
}

void reset_counters() {
  s_stats.decoded        = 0;
  s_stats.repeats        = 0;
  s_stats.unknown        = 0;
  s_stats.parity_failed  = 0;
  s_stats.overflows      = 0;
  s_stats.addr_rejected  = 0;
  s_stats.unmapped       = 0;
  s_stats.accepted       = 0;
  // Intentionally leave `last` populated so a reset between POC test
  // windows doesn't blank the most-recent-press readout.
}

void set_dispatch(const DispatchEntry* table, uint8_t count,
                  uint16_t expected_address) {
  if (count > kMaxDispatchEntries) count = kMaxDispatchEntries;
  s_dispatch_table   = table;
  s_dispatch_count   = count;
  s_expected_address = expected_address;
  Serial.print("[ir] dispatch installed entries=");
  Serial.print(static_cast<int>(count));
  Serial.print(" addr=0x");
  Serial.println(static_cast<unsigned>(expected_address), HEX);
}

void set_dispatch_enabled(bool enabled) {
  if (s_dispatch_enabled == enabled) return;
  s_dispatch_enabled = enabled;
  Serial.print("[ir] dispatch ");
  Serial.println(enabled ? "enabled" : "disabled");
}

}  // namespace ir_remote

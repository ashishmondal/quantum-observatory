// IR remote receiver — POC implementation.
//
// IMPORTANT: IRremote v4 ships its implementation inside `IRremote.hpp`
// (header-only-with-impl), so this header MUST be included from exactly
// one .cpp in the project. Other modules consume `ir_remote.h` only.
//
// Bound on Core 0; see ir_remote.h for the full rationale.

#include <Arduino.h>

#include "config.h"
#include "ir_remote.h"

// Disable the library's debug Serial chatter (one line per frame on
// the global Serial port — would race Core 1's Protomatter timing if
// we ever moved this to Core 1, and is just noise on Core 0).
#define NO_LED_FEEDBACK_CODE
#include <IRremote.hpp>

namespace ir_remote {

namespace {

Stats s_stats{};
bool  s_begun = false;

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
  // Intentionally leave `last` populated so a reset between POC test
  // windows doesn't blank the most-recent-press readout.
}

}  // namespace ir_remote

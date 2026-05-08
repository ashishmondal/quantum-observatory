// IR remote receiver — POC (phase IR.1).
//
// On-board 38 kHz IR demodulator on GP28 (silkscreen "IRM"). Bound on
// Core 0 from setup() so the IRremote library's pin-change ISR + µs
// timer cannot preempt Core 1's render loop mid-frame. See HARDWARE.md
// "IR receiver" section for the wiring + library rationale.
//
// Logging-only for now: poll() drains decoded frames into per-protocol
// counters and stashes the most recent decode for the 1 Hz log line.
// No coupling to scene_state yet — that lands once the EMI behaviour
// vs. a bright HUB75 frame has been characterised in the field
// (PLAN.md phase IR.1 pass criteria).

#pragma once

#include <stdint.h>

namespace ir_remote {

// Last successfully-decoded frame. `protocol` follows the IRremote
// `decode_type_t` enum (UNKNOWN=0, NEC=8, SONY=9, RC5=2, …); we keep
// it as a plain uint8_t so this header doesn't have to drag the
// library's internal headers into every translation unit that wants
// to print stats.
struct LastDecode {
  uint8_t  protocol;       // decode_type_t cast to uint8_t
  uint16_t address;
  uint16_t command;
  uint32_t raw_data;       // low 32 bits of the protocol-specific payload
  uint8_t  num_bits;
  uint8_t  flags;          // IRDATA_FLAGS_* bitmask (REPEAT, PARITY_FAILED, …)
  uint32_t at_ms;          // millis() when decoded
};

struct Stats {
  uint32_t   decoded;        // total successful decodes (including REPEATs)
  uint32_t   repeats;        // subset of decoded with IRDATA_FLAGS_IS_REPEAT
  uint32_t   unknown;        // protocol == UNKNOWN — likely EMI / non-IR noise
  uint32_t   parity_failed;  // IRDATA_FLAGS_PARITY_FAILED — corrupted frame
  uint32_t   overflows;      // raw-buffer overflows (host loop too slow)
  LastDecode last;           // last successful decode (decoded > 0 ⇒ valid)
};

// One-time bring-up. Call from setup() on Core 0 BEFORE the first
// poll(). Idempotent; safe to call once per boot only.
void begin();

// Drain the decoder. Cheap when no frame is pending (one volatile
// read of the library's flag). Call from loop() on Core 0 every
// iteration. Returns true if at least one frame was decoded this
// call (useful for edge-triggered logging).
bool poll();

// Snapshot of running counters. Cheap (one struct copy). Safe to
// call from Core 0; not Core-1-safe (counters are not seqlock-
// protected — they're for human-readable logs only at this stage).
Stats stats();

// Reset the running counters. Useful for the POC's per-test windows
// (panel-off / black-scene / bright-scene). Does NOT clear `last`.
void reset_counters();

}  // namespace ir_remote

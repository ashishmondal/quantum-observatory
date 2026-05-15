// IR remote receiver — Core 0 input path.
//
// On-board 38 kHz IR demodulator on GP28 (silkscreen "IRM"). Bound on
// Core 0 from setup() so the IRremote library's pin-change ISR + µs
// timer cannot preempt Core 1's render loop mid-frame. See HARDWARE.md
// "IR receiver" section for the wiring + library rationale.
//
// IR.1: poll() drained decoded frames into per-protocol counters and
//       stashed the most recent decode for diagnostics — no action.
// IR.3: poll() now also runs the FR-17.2 discipline filter (NEC only,
//       no parity / overflow), the FR-17.3 expected-address gate, and
//       a fixed dispatch table installed by the host (set_dispatch).
//       Frames that pass every gate AND match a dispatch entry trigger
//       a local-fast action and bump `accepted`; frames that pass the
//       gates but don't match any entry bump `unmapped`; frames that
//       fail the address gate bump `addr_rejected`. Lower-level
//       counters (decoded / unknown / parity_failed / overflows) keep
//       the same semantics they had in IR.1.

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
  // FR-17.3 / FR-17.5 — frames that survived NEC + parity + overflow
  // checks but were rejected by the expected-address gate. Counted
  // separately from `unknown` so a noisy neighbour TV remote shows up
  // as a distinct number from straight EMI.
  uint32_t   addr_rejected;
  // FR-17.5 — frames that passed every gate but matched no entry in
  // the installed dispatch table. Either the remote model has more
  // buttons than we mapped, or the captured cmd table is stale.
  uint32_t   unmapped;
  // FR-17.7 spirit — frames that passed every gate AND fired a
  // dispatch action this boot. The visible "did the press take?"
  // signal is the action itself (scene swap, theme cycle); this
  // counter is the offline cross-check.
  uint32_t   accepted;
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

// ---- FR-17.5 dispatch table ---------------------------------------------
//
// Each accepted IR press resolves through a fixed table installed by
// the host (main.cpp wires it up in setup() with a static array). The
// table is consulted on every decoded frame that passes the FR-17.2
// discipline filter and the FR-17.3 expected-address gate; entries
// match against the NEC command byte. Capacity is fixed (kMaxDispatchEntries)
// to keep the lookup branchless-ish and statically sized.

enum class Lane : uint8_t {
  LOCAL = 0,   // FR-17.5 local-fast — handled on Core 0 immediately
  MQTT  = 1,   // FR-17.5 mqtt-routed — published to HA, no local action
                // (MQTT echo wiring lands in IR.6; today MQTT entries
                //  just bump `accepted` so the dispatch table can be
                //  declared in full from IR.3 onward without behaviour)
};

// Action callback signature. address/command are passed through so an
// action can disambiguate when one entry covers multiple buttons (not
// used today but cheap to keep). Actions run on Core 0 inside poll() —
// keep them short; e.g. a scene_state::request() call is fine, a
// blocking MQTT publish is not (use the cross-core sentinel-0 buffer
// instead — IR.6 territory).
using ActionFn = void (*)(uint16_t address, uint16_t command);

struct DispatchEntry {
  uint16_t    command;          // NEC command byte to match
  Lane        lane;
  bool        honour_repeats;   // FR-17.4 — false for stepped actions
  ActionFn    action;           // may be nullptr for MQTT-only entries
  const char* button;           // human-readable name for logs (FR-17.7 echo)
};

// Maximum dispatch table size. Linear scan is fine at this scale; the
// cap is a static-sizing convenience for the host's table declaration.
constexpr uint8_t kMaxDispatchEntries = 16;

// Install (or replace) the dispatch table. `table` MUST outlive the
// program (use a file-scope static constexpr array). `count` is
// silently clamped to kMaxDispatchEntries. `expected_address` is the
// FR-17.3 gate — frames whose NEC address byte differs are dropped
// into the `addr_rejected` counter and never reach the table.
//
// Pass `table=nullptr, count=0` to disable dispatch entirely; the
// filter + counters keep working, just no actions fire.
void set_dispatch(const DispatchEntry* table, uint8_t count,
                  uint16_t expected_address);

// Temporarily suppress dispatch without losing the installed table.
// Used by IrTestScene's IR.2 learning wizard to keep its captures
// from accidentally cycling scenes / triggering Back / etc., which
// would yank the operator out of the wizard mid-sequence. Counters
// (incl. `accepted` for the dispatched-and-fired count) freeze in
// place while disabled — `addr_rejected` and `unmapped` are unaffected
// because they're decided before the enable check. Defaults to true.
void set_dispatch_enabled(bool enabled);

}  // namespace ir_remote

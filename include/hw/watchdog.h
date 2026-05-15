// NFR-3.2 hardware watchdog. RP2040 has a single 8 s WDT. Both cores'
// liveness must keep it fed:
//   - Core 1 publishes its heartbeat to a `volatile uint32_t` every
//     loop1() iteration (before the frame cap can early-return).
//   - Core 0 calls watchdog::tick() from loop() and only re-arms the
//     hardware when Core 1's heartbeat is fresh.
//
// A stall on either core trips a reset.

#pragma once

#include <stdint.h>

namespace watchdog {

// Arm the hardware WDT. Call once from setup(), AFTER the cross-core
// handshake (so the boot path itself can't race the WDT) but BEFORE
// any non-blocking radio bring-up (so that's also covered).
void begin();

// Feed the WDT iff Core 1's heartbeat (`render_alive_ms`) is fresh.
// During the boot window (heartbeat == 0) always feeds so setup1()'s
// FM6126A init can't trip the timer.
void tick(uint32_t now_ms, uint32_t render_alive_ms);

}  // namespace watchdog

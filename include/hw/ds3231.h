// DS3231 real-time clock driver — battery-backed authoritative time source
// for FR-9.5. Wraps the Waveshare carrier's on-board chip on I²C1
// (GP6 SDA / GP7 SCL, addr 0x68). All math is integer (NFR-1.3); no
// dynamic allocation (NFR-2.2).
//
// Convention: epoch values exchanged by this module are *local-time*
// seconds since 1970-01-01 00:00 (i.e. the BCD fields on the chip are
// stored as local wall-clock). This matches Phase 3.6.3's "store local
// in RTC" decision and keeps tz state out of every read. Callers that
// need UTC must apply tz themselves.
//
// (added in phase 3.6.1)

#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace ds3231 {

// One-time init. Brings up Wire1 on the configured pins, clears the
// oscillator-stop flag if you choose to (we DON'T here — FR-9.6 needs
// to observe it on first boot after a backup-power loss). Idempotent.
void begin();

// Read the chip's 7 BCD time registers and decode to a local-time epoch
// (see header preamble). Returns true on a successful I²C transaction.
// On false, *epoch_local is left untouched.
bool read(int32_t* epoch_local);

// Write a local-time epoch into the chip's time registers. Returns true
// on a successful I²C transaction. After a successful write, the
// oscillator-stop flag SHOULD be cleared (otherwise FR-9.6 will keep
// masking the readout) — call clear_oscillator_stopped() explicitly.
bool write(int32_t epoch_local);

// Status register 0x0F bit 7 — set by the chip whenever the oscillator
// has actually stopped (typically a backup-battery loss). Used by tod
// per FR-9.6 to keep the display masked even if a stale value reads
// successfully. Returns false on I²C error (caller should treat that
// as "unknown — assume invalid").
bool oscillator_stopped();

// Explicitly clear the OSF bit. Call after a fresh write() once you
// trust the value.
void clear_oscillator_stopped();

// On-die temperature read. Reg 0x11 holds integer °C as a signed
// 8-bit value (chip auto-converts every ~64 s; no need to force a
// conversion). Reg 0x12 has 0.25°C fractional bits in [7:6] — we
// throw it away (matches the vendor demo and is plenty for the FR-7.3
// thermal-safe trip threshold). Returns true on a successful I²C
// transaction; on false *celsius is left untouched. Note: this is
// the silicon temperature, NOT the panel surface — expect a
// significant offset (Open Question §9.8).
// (added in phase 5.5.2)
bool read_temp_c(int8_t* celsius);

}  // namespace ds3231

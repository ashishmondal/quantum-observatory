// FM6126A / ICN2038 unlock + register init for the Waveshare P3 64x32
// HUB75 panel. MUST run on the same core as Adafruit_Protomatter::begin()
// (Core 1) and BEFORE that call — Protomatter takes over the pins.
//
// Ported verbatim from the Waveshare Pico C++ SDK demo
// driver_RGBMatrix.cpp::picoRGBMatrixDeviceInit(). See FR-8.

#pragma once

namespace fm6126a {

// Bit-bangs control registers C12 and C13 over the panel pins. Reads
// pin assignments from include/config.h. Idempotent; safe to re-run.
void init();

}  // namespace fm6126a

// Single source of truth for HUB75 pin assignments and panel geometry.
// All other modules MUST consume these — no magic numbers in .cpp files.
// (NFR-5.2)
//
// Wiring matches the Waveshare "Pico-RGB-Matrix-P3-64x32-Demo" Pico C++ SDK
// driver (driver_RGBMatrix.h). This is the wiring known to work with the
// Waveshare RGB-Matrix-P3-64x32 (FM6126A) panel; do not reorder without
// reconfirming the FM6126A init still runs against the right pins.

#pragma once

// ---- HUB75 data pins ------------------------------------------------------
#define PIN_R1   2
#define PIN_G1   3
#define PIN_B1   4
#define PIN_R2   5
#define PIN_G2   8
#define PIN_B2   9

// ---- HUB75 row-address pins ----------------------------------------------
#define PIN_A    10
#define PIN_B    16
#define PIN_C    18
#define PIN_D    20
// E is unused by a 1/16-scan 64x32 panel but the FM6126A unlock sequence
// still drives it low for safety.
#define PIN_E    22

// ---- HUB75 control pins ---------------------------------------------------
#define PIN_CLK  11
#define PIN_STB  12  // a.k.a. LAT
#define PIN_OE   13

// ---- Panel geometry -------------------------------------------------------
#define PANEL_WIDTH      64
#define PANEL_HEIGHT     32
// Bit depth = bits per RGB channel (Protomatter halves this for the 6-bit
// green). Higher = more brightness levels at the cost of SRAM (buffer
// scales linearly) and a tiny refresh-rate hit. 6 gives 64 levels per
// channel, which is what AA/trail fade math needs to not collapse to 0
// at low weights. (raised from 4 in phase 2.3 motion-blur work)
#define PANEL_BIT_DEPTH  6
#define PANEL_CHAINS     1
#define PANEL_ADDR_LINES 4   // 1/16 scan
// Double-buffer the panel: scenes draw into a back buffer; show() swaps it
// in atomically. Costs ~2 KB extra SRAM (64*32 * 4-bit * 2) but eliminates
// the horizontal-line tearing seen with single-buffer rendering. Well within
// the NFR-2.1 budget. (added in phase 2.2 — tearing fix)
#define PANEL_DOUBLE_BUFFER true

// ---- DS3231 RTC (I²C1) ---------------------------------------------------
// Wired on the Waveshare carrier; battery-backed authoritative time
// source per FR-9.5. (added in phase 3.6.1)
#define PIN_RTC_SDA      6
#define PIN_RTC_SCL      7
#define RTC_I2C_HZ       400000
#define DS3231_I2C_ADDR  0x68

// ---- Photoresistor / ambient light (ADC0 / GP26) -------------------------
// Drives FR-7.1/FR-7.2 night-mode swap. 12-bit ADC; on this board
// HIGHER raw values = DARKER (LDR is the upper leg of a divider, so
// darkness = high resistance = ADC pulled toward Vref). Calibrated
// in-enclosure 2026-05: lights on ≈ 2700, finger over sensor ≈ 3800,
// room lights off ≈ 4000. Threshold 3500 trips between "finger" and
// "dark room"; hysteresis 200 prevents edge flicker. Tunable live via
// MQTT once 5.5.3 lands. (added in phase 5.5.1)
#define PIN_LIGHT_SENSOR              26
#define LIGHT_SENSOR_ADC_INPUT        0     // ADC channel 0 maps to GP26
#define LIGHT_NIGHT_THRESHOLD_DEFAULT 3500
#define LIGHT_NIGHT_HYSTERESIS_DEFAULT 200

// ---- DS3231 on-die temperature (FR-7.3 thermal safety) -------------------
// Silicon temp, NOT panel surface — expect a meaningful offset (Open
// Question §9.8 will correlate against an IR thermometer reading).
// Defaults are conservative placeholders: 50°C trips well above any
// reasonable ambient (so the device can sit in a closed room) but well
// below LED damage; 5°C hysteresis prevents oscillation around the
// edge. Tune from `[thermal] c=` logs once we have soak data; tunable
// live via MQTT once 5.5.3 lands. (added in phase 5.5.2)
#define THERMAL_THRESHOLD_C_DEFAULT  50
#define THERMAL_HYSTERESIS_C_DEFAULT  5

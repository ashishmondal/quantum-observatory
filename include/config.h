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
// Bit depth = bits per RGB channel. Higher = more brightness levels but
// LOWER refresh rate (BCM total period scales as 2^N). On RP2040 + 64x32:
//   4 -> ~280 Hz   (16 levels/ch)
//   5 -> ~190 Hz   (32 levels/ch)
//   6 -> ~110 Hz   (64 levels/ch — borderline visible flicker)
// Dropped 6 -> 5 to eliminate a perceptible shimmer. Trail/AA fade math
// still works at 32 levels; only the dimmest 1–2 steps collapse, which
// the gamma curve in starfield_bg.h already compensates for.
#define PANEL_BIT_DEPTH  5
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
#define LIGHT_NIGHT_THRESHOLD_DEFAULT 3800
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

// ---- IR receiver (GP28 / IRM) --------------------------------------------
// On-board 38 kHz IR demodulator wired to GP28 (silkscreen "IRM") on the
// Waveshare carrier. Output is active-low, already squared by the
// receiver's internal AGC + bandpass — no carrier demodulation needed in
// firmware. Driven by the IRremote v4 library, whose ISR attaches a
// pin-change interrupt and a microsecond timer; steady-state cost is zero
// because the ISR fires only on IR edges (~30 edges per NEC frame, all
// within ~70 ms of a button press).
//
// Bound on Core 0 from setup() so Core 1's render loop can't be preempted
// mid-frame. Bare logging-only POC for now (phase IR.1) — once the EMI
// behaviour vs. a bright HUB75 frame is characterised, decoded events
// will feed scene_state alongside the buttons.
#define PIN_IR_RX  28

// ---- Observer location (sun position) ------------------------------------
// Drives the sky-gradient + sun-arc background on the giant clock.
// Default: Houston, TX. Will become MQTT-settable in a later phase.
// LATITUDE_DEG  +N / -S, LONGITUDE_DEG +E / -W.
// LOCAL_TZ_OFFSET_MIN: minutes offset from UTC (Houston CST = -360,
// CDT = -300). Currently we display RTC local time directly; the sun
// math wants UTC, so we subtract this offset back out. Update when
// daylight-saving flips until MQTT pushes a tz string.
#define LATITUDE_DEG          29.7604f
#define LONGITUDE_DEG        -95.3698f
#define LOCAL_TZ_OFFSET_MIN  -300       // CDT (UTC-5); use -360 for CST

// ---- Compositor idle-slack budget (FR-16.9) ------------------------------
// Per-frame headroom = `kFrameIntervalMs - render_time` (Core 1, integer
// ms). The rolling average is published as `g_render_slack_ms` and
// surfaced in the observatory/status heartbeat so HA can size new
// layers / heavier scenes against actual measured budget.
//
// `kSlackFloorMs` is the minimum reported slack required before
// optional Core-1 work runs (D.6 sky-model, D.7 Scene::prepare()).
// Below the floor, those features SHALL skip the frame to preserve
// FR-3.1's frame-rate target. The floor is intentionally generous (~20%
// of the 42 ms frame budget) so a heavy scene can still spend the full
// frame on itself without speculative work piling on. Consumers land in
// D.6 and later — D.5 only publishes the measurement.
#define RENDER_SLACK_FLOOR_MS_DEFAULT  8

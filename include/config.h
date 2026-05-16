// Single source of truth for HUB75 pin assignments and panel geometry.
// All other modules MUST consume these — no magic numbers in .cpp files.
// (NFR-5.2)
//
// Wiring matches the Waveshare "Pico-RGB-Matrix-P3-64x32-Demo" Pico C++ SDK
// driver (driver_RGBMatrix.h). This is the wiring known to work with the
// Waveshare RGB-Matrix-P3-64x32 (FM6126A) panel; do not reorder without
// reconfirming the FM6126A init still runs against the right pins.

#pragma once

#include "scene_state.h"  // SceneId — needed for kRemoteCycle[] (FR-17.6)

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

// ---- Piezo buzzer (GP27, active-high) ------------------------------------
// On-board buzzer on the Waveshare carrier (HARDWARE.md "Buzzer"). Driven
// as a passive piezo via Arduino tone() so we get pitch control; works on
// an active buzzer too (carrier just rectifies the PWM into its fixed
// pitch). All firmware tones stay above 8 kHz — see buzzer.h for the
// rationale and the chirp envelope.
#define PIN_BUZZER  27

// ---- On-board buttons (FR-11.1) ------------------------------------------
// Three carrier-side push-buttons wired active-low. v1 firmware only
// uses MENU; UP/DOWN are reserved for future local actions per
// FR-11.3. We define all three so future scopes can `pinMode()` them
// without re-discovering the pin map.
//   GP15 = MENU  → drives the FR-17.8 / IR.4 info overlay (FR-19
//                  rebinding: the IR remote OK button is now used to
//                  commit the FR-19 settings menu, so info overlay
//                  needed its own dedicated physical button).
//   GP19 = DOWN  → reserved.
//   GP21 = UP    → reserved.
// External pull-ups are populated on the carrier; the firmware also
// enables the RP2040 internal pull-up defensively (parallel pulls
// don't hurt; protects against an unpopulated board).
#define PIN_BTN_MENU 15
#define PIN_BTN_DOWN 19
#define PIN_BTN_UP   21

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

// ---- IR remote button mapping (FR-17.3) ----------------------------------
// Captured 2026-05 against the target Roku-style remote using the on-panel
// IR-learning wizard (IrTestScene → observatory/debug). All eight buttons
// share the same NEC address; the dispatch table in phase IR.3 will reject
// any frame whose address byte doesn't match `IR_REMOTE_ADDR_EXPECTED`.
//
// Note: 0xC2EA is a 16-bit "extended NEC" address (the second address byte
// is NOT the bitwise inverse of the first, so IRremote v4 reports the full
// 16-bit pair as `decodedIRData.address` instead of an 8-bit value). The
// FR-17.3 gate compares against the same 16-bit field, so the wider type
// is what we want.
//
// REPLAY is intentionally absent — this particular Roku remote doesn't ship
// with that key. The dispatch table in IR.3 simply omits it.
#define IR_REMOTE_ADDR_EXPECTED  0xC2EA  // = 49898 dec

#define kIrButtonHomeCmd     3
#define kIrButtonUpCmd      25
#define kIrButtonDownCmd    51
#define kIrButtonLeftCmd    30
#define kIrButtonRightCmd   45
#define kIrButtonOkCmd      42
#define kIrButtonBackCmd   102
#define kIrButtonOptionsCmd 97

// FR-17.6 — operator-facing scene cycle list for ▲/▼ on the IR remote.
// Excludes firmware-owned overrides (BOOT, NIGHT, THERMAL_SAFE, OFFLINE,
// SPLASH) and diagnostic scenes (GFX_TEST, IR_TEST) by design — these
// shouldn't be reachable by accident from the couch. Append-only;
// reorder = behaviour change for anyone who's memorised "▲ ▲ ▲ = Jupiter".
//
// Defined here (not in main.cpp) so future input modes (on-board
// buttons FR-11, voice, etc.) can share the same list without
// duplicating the policy. `inline constexpr` (C++17) gives the array
// external linkage with one copy across all TUs that include this
// header — `static constexpr` would emit a copy per .cpp.
inline constexpr scene_state::SceneId kRemoteCycle[] = {
    scene_state::SceneId::CLOCK,
    scene_state::SceneId::MOON_PHASE,
    scene_state::SceneId::JUPITER_VISIBILITY,
    scene_state::SceneId::CONSTELLATION_NOW,
    scene_state::SceneId::ISS_PASS,
};
inline constexpr uint8_t kRemoteCycleCount =
    sizeof(kRemoteCycle) / sizeof(kRemoteCycle[0]);

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

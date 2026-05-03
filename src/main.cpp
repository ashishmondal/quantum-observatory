#include <Arduino.h>
#include <Adafruit_Protomatter.h>

#include "config.h"
#include "backgrounds.h"
#include "ds3231.h"
#include "fixed_point.h"
#include "gfx_text.h"
#include "light_sensor.h"
#include "scene_state.h"
#include "thermal_monitor.h"
#include "time_of_day.h"
#include "wifi_link.h"
#include "mqtt_link.h"
#include "scenes/scene.h"
#include "scenes/background_scene.h"
#include "scenes/boot_scene.h"
#include "scenes/clock_scene.h"
#include "scenes/color_cycle_scene.h"
#include "scenes/giant_clock_scene.h"
#include "scenes/night_scene.h"
#include "scenes/text_demo_scene.h"
#include "scenes/thermal_safe_scene.h"
uint8_t rgbPins[]  = {PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2};
uint8_t addrPins[] = {PIN_A,  PIN_B,  PIN_C,  PIN_D};
uint8_t clockPin   = PIN_CLK;
uint8_t latchPin   = PIN_STB;
uint8_t oePin      = PIN_OE;

Adafruit_Protomatter matrix(
    PANEL_WIDTH,
    PANEL_BIT_DEPTH,
    PANEL_CHAINS,     rgbPins,
    PANEL_ADDR_LINES, addrPins,
    clockPin, latchPin, oePin,
    PANEL_DOUBLE_BUFFER
);

// Static scene instances — never heap-allocated (NFR-2.2). Add new scenes
// here as plain file-scope objects, then point g_current_scene at the one
// you want active. The MQTT-driven dispatcher (`scene_for()`, phase 5.4)
// reaches these by SceneId; instances marked `[[maybe_unused]]` are not
// reached from any SceneId case yet but are kept linker-alive for the
// next phase that wires them in.
[[maybe_unused]] static BootScene             s_boot_scene;        // phase 1.5 demo
[[maybe_unused]] static ClockScene            s_clock_scene;       // phase 1.4 demo
[[maybe_unused]] static ColorCycleScene       s_color_cycle_scene; // smoke-test fallback

// One BackgroundScene per BgType — each is just a thin wrapper that
// dispatches to g_backgrounds. Adding a fourth bg type means: implement
// *Bg.h, add enum value + dispatch case in backgrounds.h, declare another
// instance here. (NFR-5.1 spirit, scaled down from full scenes.)
static BackgroundScene s_bg_starfield(BgType::STARFIELD);
static BackgroundScene s_bg_parallax(BgType::PARALLAX);
static BackgroundScene s_bg_nebula  (BgType::NEBULA);
[[maybe_unused]] static TextDemoScene s_text_demo_scene;
static GiantClockScene s_giant_clock_scene; // phase 3.5.3 — default room-clock view
static NightScene      s_night_scene;       // phase 5.5.1 — LDR-triggered override
static ThermalSafeScene s_thermal_safe_scene; // phase 5.5.2 — DS3231-triggered override

// Single "current scene" pointer; loop() just delegates to it. Swapping
// scenes is one assignment — no other code changes. (NFR-5.1)
static Scene* g_current_scene = &s_giant_clock_scene;

// Phase 4.2 dispatcher — maps a stable SceneId to one of the file-scope
// Scene instances above. Returns nullptr for unknown ids (defensive
// default for FR-1.3: malformed traffic must not crash). The list grows
// in lockstep with scene_state::SceneId; per NFR-5.1 adding a scene is
// (a) one new SceneId enum value + (b) one case here.
static Scene* scene_for(scene_state::SceneId id) {
  using SI = scene_state::SceneId;
  switch (id) {
    case SI::BOOT:         return &s_boot_scene;
    case SI::CLOCK:        return &s_giant_clock_scene;
    case SI::COLOR_CYCLE:  return &s_color_cycle_scene;
    case SI::TEXT_DEMO:    return &s_text_demo_scene;
    case SI::BG_STARFIELD: return &s_bg_starfield;
    case SI::BG_PARALLAX:  return &s_bg_parallax;
    case SI::BG_NEBULA:    return &s_bg_nebula;
    case SI::NIGHT:        return &s_night_scene;
    case SI::THERMAL_SAFE: return &s_thermal_safe_scene;
  }
  return nullptr;
}

// FM6126A / ICN2038 init sequence (required by the Waveshare P3 64x32 panel
// before any image will appear). Ported verbatim from the working Waveshare
// Pico C++ SDK demo driver_RGBMatrix.cpp::picoRGBMatrixDeviceInit().
// Writes control registers 12 and 13.
static void fm6126a_init() {
  const uint8_t rgb[6] = {PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2};
  const uint8_t addr[5] = {PIN_A, PIN_B, PIN_C, PIN_D, PIN_E};

  for (uint8_t p : rgb)  { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  for (uint8_t p : addr) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
  pinMode(PIN_CLK, OUTPUT); digitalWrite(PIN_CLK, LOW);
  pinMode(PIN_STB, OUTPUT); digitalWrite(PIN_STB, LOW);
  pinMode(PIN_OE,  OUTPUT); digitalWrite(PIN_OE,  HIGH); // blank

  const int MaxLed = 64;
  const int C12[16] = {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
  const int C13[16] = {0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0};

  // Register 12
  for (int l = 0; l < MaxLed; l++) {
    int y = l % 16;
    int v = C12[y] ? HIGH : LOW;
    for (uint8_t p : rgb) digitalWrite(p, v);
    digitalWrite(PIN_STB, (l > MaxLed - 12) ? HIGH : LOW);
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_CLK, LOW);
  }
  digitalWrite(PIN_STB, LOW);
  digitalWrite(PIN_CLK, LOW);

  // Register 13
  for (int l = 0; l < MaxLed; l++) {
    int y = l % 16;
    int v = C13[y] ? HIGH : LOW;
    for (uint8_t p : rgb) digitalWrite(p, v);
    digitalWrite(PIN_STB, (l > MaxLed - 13) ? HIGH : LOW);
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(2);
    digitalWrite(PIN_CLK, LOW);
  }
  digitalWrite(PIN_STB, LOW);
  digitalWrite(PIN_CLK, LOW);
}

ProtomatterStatus g_status = PROTOMATTER_ERR_PINS;

// Core 0 → Core 1 handshake. setup() (Core 0) does Serial + fp LUT init,
// then sets this flag. setup1() (Core 1) busy-waits on it before touching
// the matrix or any scene that reads the LUT.
//
// We use a plain volatile bool rather than a mutex/FIFO because (a) the
// write happens exactly once before any reader spins, (b) RP2040 is
// single-issue in-order so a naturally-aligned bool write is atomic, and
// (c) no other shared state crosses cores yet — that arrives in 4.2 with
// SceneState + mutex_t. (added in phase 4.1)
static volatile bool s_core0_ready = false;

void setup() {
  Serial.begin(115200);

  // Build the integer trig LUT before any scene that uses it. (NFR-1.3)
  // Lives on Core 0 because it's a one-shot init and Core 1 spins waiting
  // for s_core0_ready before reading it.
  fp::sin_cos_lut_init();

  // Cross-core scene IPC — must be live before either core touches
  // scene_state. Default current/pending = BOOT; loop1() will resolve
  // that to whatever scene_for(BOOT) returns at startup. (phase 4.2)
  scene_state::init();
  scene_state::request(scene_state::SceneId::CLOCK);  // default idle

  // Bring up the shared TimeOfDay state (FR-9.5). Just inits the
  // mutex; the actual time comes from the RTC via tod::poll() in
  // loop(). Stays invalid ("--:--") until the first successful poll
  // against an RTC with the oscillator-stop flag clear (FR-9.6).
  tod::init();

  // DS3231 RTC bring-up (FR-9.5). Battery-backed authoritative time
  // source — every reader (chrome, giant clock, future scenes) goes
  // through tod::now() which reads the cache populated by tod::poll().
  ds3231::begin();

  // Photoresistor bring-up (FR-7.1). Polled at ~1 Hz from loop();
  // transitions publish into scene_state::set_night_active() which
  // makes the renderer swap to the dim NIGHT scene (FR-7.2).
  light_sensor::begin();

  // DS3231 on-die temperature monitor (FR-7.3). Polled at 0.1 Hz from
  // loop(); transitions publish into scene_state::set_thermal_active()
  // which preempts everything else (FR-7.5) with THERMAL_SAFE.
  thermal_monitor::begin();

  // Optional one-shot bootstrap. Define RTC_SEED_LOCAL_EPOCH (e.g. via
  // platformio.ini build_flags or secrets.h) to seed the chip with a
  // local-time epoch on this boot, then REMOVE the define and reflash
  // so subsequent boots leave the (battery-backed) RTC alone. Useful
  // for a fresh DS3231 with no MQTT broker available; once HA is up,
  // the observatory/time topic (FR-9.5 / phase 5.6) handles
  // corrections without reflashing. Pick the value with:
  //   date -d '2026-05-01 19:30:00' +%s
#ifdef RTC_SEED_LOCAL_EPOCH
  if (ds3231::write(static_cast<int32_t>(RTC_SEED_LOCAL_EPOCH))) {
    ds3231::clear_oscillator_stopped();
    Serial.print("[boot] rtc seeded local_epoch=");
    Serial.println(static_cast<long>(RTC_SEED_LOCAL_EPOCH));
  } else {
    Serial.println("[boot] rtc seed FAILED (i2c)");
  }
#endif

  Serial.println("[boot] core0 ready, releasing core1");
  s_core0_ready = true;

  // Kick off Wi-Fi after the cross-core handshake so any radio init
  // serial chatter doesn't race the [boot] line. Non-blocking — the
  // state machine in wifi_link::poll() takes it from here. (FR-5.2)
  wifi_link::begin();
  // MQTT layers on top — it'll sit in WAIT_WIFI until the link is up.
  mqtt_link::begin();
}

void loop() {
  // Core 0 — Gatekeeper. Owns Wi-Fi + MQTT and the 1 Hz tod cache
  // refresh. Scene changes now arrive via observatory/scene → the
  // mqtt_link callback calls scene_state::request() directly, so this
  // loop no longer fakes traffic. (Phase 5.4 ripped out the demo
  // CLOCK ↔ BG_NEBULA toggle that previously lived here.)
  static uint32_t last_print_ms = 0;
  const uint32_t now_ms = millis();

  // Drive the Wi-Fi state machine every loop iteration. Cheap when
  // nothing is changing; transitions print one line each.
  wifi_link::poll(now_ms);
  mqtt_link::poll(now_ms);

  // Photoresistor poll (FR-7.1). Internally rate-limited to ~1 Hz.
  // On a night-state transition, push the new flag into scene_state
  // so the renderer's dispatcher picks NIGHT vs the Director's choice
  // on the next frame (FR-7.2 / FR-7.5).
  if (light_sensor::poll(now_ms)) {
    scene_state::set_night_active(light_sensor::is_night());
  }

  // DS3231 temp poll (FR-7.3). Internally rate-limited to 0.1 Hz.
  // On a hot-state transition, push the new flag into scene_state;
  // FR-7.5 priority means THERMAL_SAFE preempts NIGHT and any
  // Director request the next frame.
  if (thermal_monitor::poll(now_ms)) {
    scene_state::set_thermal_active(thermal_monitor::is_hot());
  }

  if (now_ms - last_print_ms >= 1000u) {
    last_print_ms = now_ms;
    tod::poll(now_ms);
    const tod::Reading r = tod::now(now_ms);
    if (r.valid) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
               r.hour, r.minute, r.second);
      Serial.print("[time] "); Serial.println(buf);
    } else {
      // FR-9.6: RTC unread or oscillator-stop flag set.
      Serial.println("[time] --:--:-- (rtc invalid)");
    }

    // Per-second on-die temp readout for thermal-threshold calibration
    // (Open Question §9.8 — DS3231 silicon vs. panel surface). Remove
    // once §9.8 closes (Phase 9.4 IR-thermometer correlation).
    int8_t temp_c;
    if (ds3231::read_temp_c(&temp_c)) {
      Serial.print("[thermal] c=");
      Serial.println(static_cast<int>(temp_c));
    } else {
      Serial.println("[thermal] read FAILED");
    }
  }

  // Phase 4.3 stress test: hammer Core 0 with a CPU-bound loop that
  // simulates the JSON-parse + checksum cost a real MQTT message would
  // incur (NFR-1.1 / NFR-3.1 validation). Off by default; enable with
  //   build_flags = -DCORE0_STRESS
  // and watch [render] fps=… on Core 1 — it MUST stay flat vs. baseline.
  // The buffer is sized to the documented Scene Contract budget
  // (NFR-2.3: 512 B for §5.1 payload + headroom). Static allocation,
  // no heap, no float (NFR-2.2 / NFR-1.3) so the workload itself can't
  // be blamed for any rendering hiccup.
#ifdef CORE0_STRESS
  {
    static uint8_t  s_stress_buf[512];
    static uint32_t s_stress_iters = 0;
    static uint32_t s_stress_last_log_ms = 0;
    // Representative payload: largest documented Scene Contract example
    // (REQUIREMENTS §5.1) padded to fill the buffer. Copying it in is
    // cheaper than memset but pulls real bytes through the cache.
    static const char kPayload[] =
        "{\"scene_id\":\"jupiter_visibility\",\"priority\":3,"
        "\"duration\":30,\"sticky\":false,"
        "\"overrides\":{\"text\":\"Visible: East @ 9PM\",\"val\":\"78\"}}";
    constexpr size_t kPayloadLen = sizeof(kPayload) - 1;
    // memcpy + a rolling checksum. ~2-3 µs per pass on RP2040 — tight
    // enough to saturate Core 0 between the 10 ms delay() calls.
    for (int i = 0; i < 200; ++i) {
      memcpy(s_stress_buf, kPayload,
             kPayloadLen < sizeof(s_stress_buf) ? kPayloadLen
                                                : sizeof(s_stress_buf));
      uint32_t sum = 0;
      for (size_t j = 0; j < sizeof(s_stress_buf); ++j) {
        sum = sum * 31u + s_stress_buf[j];
      }
      // Side-effect on a static so the optimiser can't elide the loop.
      s_stress_iters += sum;
    }
    if (now_ms - s_stress_last_log_ms >= 1000u) {
      s_stress_last_log_ms = now_ms;
      Serial.print("[stress] core0 iters=");
      Serial.println(static_cast<unsigned long>(s_stress_iters));
    }
  }
#endif

  delay(10);
}

// ─── Core 1 — Artist ─────────────────────────────────────────────────────
//
// Owns the matrix end-to-end: FM6126A init, Protomatter begin(), scene
// init, and the render+FPS loop. Protomatter binds its PIO/DMA to the
// core that calls begin(), so this MUST be Core 1 — not setup() or loop().
// (added in phase 4.1)

void setup1() {
  // Wait for Core 0 to finish Serial + fp LUT init before we use either.
  while (!s_core0_ready) {
    tight_loop_contents();
  }

  // Run the FM6126A unlock/init BEFORE Protomatter takes over the pins.
  fm6126a_init();

  g_status = matrix.begin();

  if (g_status == PROTOMATTER_OK) {
    // Seed every background's deterministic state once. Cheap; no heap.
    g_backgrounds.init_all(matrix);
  }

  if (g_status == PROTOMATTER_OK && g_current_scene != nullptr) {
    g_current_scene->init(matrix);
    Serial.print("[scene] active=");
    Serial.println(g_current_scene->name());
  }
}

void loop1() {
  // FPS counter: count frames between wall-clock seconds and print once/sec.
  // Integer math only (NFR-1.3) — no float in the render loop.
  static uint32_t frames = 0;
  static uint32_t last_report_ms = 0;

  const uint32_t now_ms = millis();

  // Phase 4.2: consume any pending scene change requested by Core 0.
  // take_pending() returns true exactly once per request(), so we only
  // re-init on actual transitions. Unknown ids leave the active scene
  // alone (FR-1.3 spirit applied at the cross-core boundary).
  scene_state::SceneId pending_id;
  if (g_status == PROTOMATTER_OK && scene_state::take_pending(&pending_id)) {
    Scene* next = scene_for(pending_id);
    if (next != nullptr && next != g_current_scene) {
      g_current_scene = next;
      g_current_scene->init(matrix);
      scene_state::mark_current(pending_id);
      Serial.print("[scene] swap -> ");
      Serial.println(g_current_scene->name());
    } else if (next == nullptr) {
      Serial.print("[scene] unknown id=");
      Serial.println(static_cast<int>(pending_id));
    }
  }

  if (g_status == PROTOMATTER_OK && g_current_scene != nullptr) {
    // 1. Scene draws background + foreground but does NOT call show().
    //    (FR-9.3 / phase 3.5.2 — chrome must overlay before flip.)
    g_current_scene->render(matrix, now_ms);

    // 2. Shared chrome on top. Currently just the always-on HH:MM clock
    //    readout (FR-9.2). Scenes opt out via wants_clock_chrome().
    if (g_current_scene->wants_clock_chrome()) {
      gfx::draw_clock_chrome(matrix, now_ms);
    }

    // 3. Single show() per frame.
    matrix.show();
  }

  frames++;

  if (now_ms - last_report_ms >= 1000u) {
    Serial.print("[render] fps=");
    Serial.print(frames);
    Serial.print(" scene=");
    Serial.print(g_current_scene ? g_current_scene->name() : "none");
    Serial.print(" protomatter_status=");
    Serial.println((int)g_status);
    frames = 0;
    last_report_ms = now_ms;
  }
}
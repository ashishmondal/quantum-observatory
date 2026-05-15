#include <Arduino.h>
#include <Adafruit_Protomatter.h>
#include <WiFi.h>

#include "config.h"
#include "fm6126a_init.h"
#include "info_status_publish.h"
#include "ir_actions.h"
#include "mqtt_edge.h"
#include "rtc_poll.h"
#include "stress_harness.h"
#include "watchdog.h"
#include "backgrounds.h"
#include "color_palette.h"
#include "ds3231.h"
#include "fixed_point.h"
#include "gfx_text.h"
#include "light_sensor.h"
#include "scene_state.h"
#include "theme.h"
#include "thermal_monitor.h"
#include "time_of_day.h"
#include "wifi_link.h"
#include "mqtt_link.h"
#include "iss_state.h"
#include "iss_geometry.h"
#include "iss_visibility.h"
#include "sun_position.h"
#include "jupiter_state.h"
#include "constellation_state.h"
#include "moon_state.h"
#include "ir_remote.h"
#include "buzzer.h"
#include "compositor.h"
#include "scene_registry.h"
#include "scenes/scene.h"
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

// Scene instances and the SceneId→Scene* dispatcher live in
// scene_registry.{h,cpp}. main.cpp resolves the active scene through
// scene_registry::scene_for() / ::default_scene() so adding a scene
// is one enum value (scene_state.h) + one switch case + one
// file-scope instance (scene_registry.cpp) — no main.cpp edit needed
// (NFR-5.1).

// Active scene + compositor layer stack + render telemetry +
// fade-swap state machine all live in compositor.{h,cpp}.
// main.cpp's loop1() body is just `compositor::tick(matrix, now_ms)`.

// ─── ISS visibility auto-switch (phase 7.1++) ────────────────────────
// Predicate + edge-detector + auto-switch live in iss_visibility.{h,cpp}
// — kept out of main.cpp so the rule has a single source of truth shared
// with IssPassScene::render.

// ─── IR remote dispatch (FR-17.5, phase IR.3) ────────────────────────
// All action_ir_* functions and the kIrDispatch[] table live in
// ir_actions.{h,cpp}. main.cpp only has to call
// ir_actions::install_dispatch_table(scene_registry::font_demo()) once
// during setup() to bind the receiver.

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

// Render telemetry (g_render_fps, g_render_alive_ms, g_render_slack_ms,
// g_first_frame_render_ms) is defined in compositor.cpp; main.cpp's
// loop() reads them through compositor.h declarations.

void setup() {
  Serial.begin(115200);

  // Build the integer trig LUT before any scene that uses it. (NFR-1.3)
  // Lives on Core 0 because it's a one-shot init and Core 1 spins waiting
  // for s_core0_ready before reading it.
  fp::sin_cos_lut_init();

  // Build the gamma-corrected color palettes (color_palette.h). Same
  // lifecycle as the trig LUT: built once on Core 0, then read-only
  // from Core 1's render path.
  palette::init_all();

  // Theming system (FR-15). Boots to APOLLO_AMBER per FR-15.2; HA may
  // push a non-default theme via observatory/theme later (T.4). The
  // explicit set() here documents intent and exercises the writer side
  // of the API; the log line proves the reader links. Single-byte
  // atomic store on RP2040 — no mutex (same pattern as g_render_fps).
  theme::set(theme::Id::APOLLO_AMBER);
  Serial.print("[theme] active id=");
  Serial.println(static_cast<int>(theme::current()));

  // Cross-core scene IPC — must be live before either core touches
  // scene_state. Default current/pending = BOOT; loop1() will resolve
  // that to whatever scene_for(BOOT) returns at startup. (phase 4.2)
  scene_state::init();
  // Default idle = giant clock at the lowest priority (0) so any MQTT
  // request, even priority 1, beats it (FR-2.1, phase 6.1).
  scene_state::request(scene_state::SceneId::CLOCK, 0);  // default idle
  // Boot splash override — preempts everything (incl. thermal_safe)
  // until the first MQTT-connected edge in loop() clears it.
  scene_state::set_splash_active(true);

  // Bring up the shared TimeOfDay state (FR-9.5). Just inits the
  // mutex; the actual time comes from the RTC via tod::poll() in
  // loop(). Stays invalid ("--:--") until the first successful poll
  // against an RTC with the oscillator-stop flag clear (FR-9.6).
  tod::init();

  // Lunar-state IPC for the moon_phase scene's MQTT data path
  // (observatory/moon). Falls back to local synodic-month math when
  // no fresh value has been pushed.
  moon_state::init();

  // ISS-pass IPC for the iss_pass scene's MQTT data path
  // (observatory/iss). Falls back to a "WAIT" placeholder when
  // no fresh value has been pushed.
  iss_state::init();

  // Jupiter-visibility IPC for the jupiter_visibility scene's MQTT
  // data path (observatory/jupiter). Falls back to a "WAIT"
  // placeholder when no fresh value has been pushed.
  jupiter_state::init();

  // Constellation selector IPC for the constellation_now scene's
  // MQTT data path (observatory/constellation). Falls back to a
  // local rotation through the catalog every 30 s when no fresh
  // value has been pushed (so the scene works standalone).
  constellation_state::init();

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

  // IR remote receiver bring-up (phase IR.1 — POC, logging only).
  // Bound on Core 0 so the IRremote library's pin-change ISR + µs
  // timer cannot preempt Core 1's render loop mid-frame. Decoded
  // events are NOT yet wired into scene_state — that lands once the
  // EMI characterisation against bright HUB75 frames is complete.
  ir_remote::begin();
  // Piezo buzzer on GP27 — used by ir_remote::poll() to chirp on
  // every accepted IR press (FR-17.5 `accepted` path). Bring up
  // BEFORE set_dispatch() so a stray frame between begin() and the
  // first loop() can't call into an uninitialised buzzer module.
  buzzer::begin();
  // FR-10.4 audible "device awake" cue — Westminster Quarters first
  // phrase (G#5, F#5, E5, B4). The bell-like descending stepwise
  // motif followed by the leap to the lower B4 reads instantly as
  // "clock chime", which fits the observatory + clock identity. Sits
  // in the C5..C7 sweet spot of the carrier piezo (4 kHz mechanical
  // resonance) so the notes actually carry the pitch instead of
  // collapsing to a single shrill beep, the way an >8 kHz melody
  // does. Total wall-clock = 4*350 + 600 = 2000 ms but the last note
  // is the longest "bong" — perceptually the chime is over by ~1.4 s
  // and the rest is just the final bell ringing out. Non-blocking
  // (buzzer::tick() in loop()) so this does not delay scene_state
  // init or first-frame render. Cancels the FR-10.4 self-test chirp
  // fired inside buzzer::begin() (play() calls stop_internal()
  // first), which is fine — the first Westminster note IS the
  // wiring witness.
  static constexpr buzzer::Note kBootMelody[] = {
      { 831, 350 },   // G#5  — "ding"
      { 740, 350 },   // F#5  — "dong"
      { 659, 350 },   // E5   — "ding"
      { 494, 600 },   // B4   — "dong" (held; bell tail)
  };
  buzzer::play(kBootMelody, sizeof(kBootMelody) / sizeof(kBootMelody[0]));
  // FR-17.5 — install the local-fast dispatch table for IR.3. Each
  // accepted press routes through scene_state::request() at priority
  // 1 / duration 120 s so a real ISS pass (priority 4) can still
  // preempt a couch-driven scene cycle (FR-17.6).
  ir_actions::install_dispatch_table(scene_registry::font_demo());

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

  // NFR-3.2 watchdog. RP2040 has a single hardware WDT — max ~8.3 s
  // on this core. Both cores' liveness must keep it fed: Core 0
  // calls wdt_reset() from loop(), but only when Core 1's heartbeat
  // (g_render_alive_ms) is fresh. So a stall on either core trips a
  // reset. Begin AFTER s_core0_ready so the boot path itself can't
  // race the WDT, but BEFORE network init so any non-blocking radio
  // bring-up is also covered.
  watchdog::begin();

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

  // IR remote drain (phase IR.1 — POC). Cheap when no frame is
  // pending; counters surface in the 1 Hz [ir] log line below.
  ir_remote::poll();

  // Drive the buzzer's non-blocking melody scheduler (FR-10.7).
  // Cheap when nothing is playing (one millis() compare). Lives
  // alongside ir_remote::poll() because the chirp path is the
  // dominant caller; theme-switch melodies (B.3) ride the same
  // tick.
  buzzer::tick(now_ms);

  // Phase 7.1++ — ISS visibility auto-switch + ta-da. Internally
  // rate-limited to 1 Hz; rising edge of (sunlit ∧ twilight ∧
  // above-horizon) requests SceneId::ISS_PASS at priority 4 sticky
  // and plays the kIssVisibleMelody. User can navigate away via
  // IR/MQTT at any time (those paths set user_intent=true so they
  // beat the sticky); a falling edge clears the sticky only when
  // ISS_PASS is still the active scene.
  iss_visibility::tick(now_ms);

  // FR-17.5 / IR.3 — the IrTestScene learning wizard (phase IR.2)
  // captures NEC frames directly off ir_remote::stats() and would be
  // dragged out of its own sequence the moment a captured ▲/▼ press
  // also fired the scene-cycle action. Disable dispatch whenever the
  // wizard is the active scene; re-enable as soon as the operator
  // leaves it. set_dispatch_enabled() is a same-state no-op so the
  // per-iteration call is essentially free.
  ir_remote::set_dispatch_enabled(
      scene_state::current() != scene_state::SceneId::IR_TEST);

  // Phase T.9 / FR-15.8 — gfx_test theme auto-cycle.
  // While the diagnostic scene is up, rotate themes every
  // kThemeStepMs so a single capture covers every theme's Ink and
  // Hint coverage. theme::cycle() is the same writer-side path the
  // MQTT and IR remote handlers already use (CODING_PRACTICES §3
  // single-writer rule for the atom-flip mode-switch pattern). No
  // work for any other scene — last_step_ms resets on exit so a
  // re-entry restarts from the current theme.
  {
    static constexpr uint32_t kThemeStepMs = 6000u;  // 5 themes × 6 s = 30 s full sweep
    static uint32_t s_last_theme_step_ms   = 0;
    if (scene_state::current() == scene_state::SceneId::GFX_TEST) {
      if (s_last_theme_step_ms == 0) s_last_theme_step_ms = now_ms;
      if (now_ms - s_last_theme_step_ms >= kThemeStepMs) {
        theme::cycle(+1);
        s_last_theme_step_ms = now_ms;
        Serial.print("[gfx_test] theme=");
        Serial.println(theme::string_from_id(theme::current()));
      }
    } else {
      s_last_theme_step_ms = 0;
    }
  }

  // Phase 6.4 / FR-5.1 — MQTT-disconnect override + boot-splash
  // dismissal. Both edge detectors live in mqtt_edge.cpp.
  mqtt_edge::tick(now_ms);

  // Phase 6.2: drive scene-lifecycle expiry (FR-2.3 duration revert,
  // FR-2.4 hard 1 h TTL). Cheap when nothing is due; on expiry,
  // reverts to the default CLOCK at priority 0.
  scene_state::tick(now_ms);

  if (now_ms - last_print_ms >= 1000u) {
    last_print_ms = now_ms;

    // Render FPS — sourced from Core 1 via g_render_fps to keep all
    // Serial output on Core 0 (Protomatter timing protection).
    Serial.print("[render] fps=");
    Serial.println(static_cast<unsigned long>(g_render_fps));

    // FR-17.8 / IR.4 — republish the diagnostic snapshot Core 1's
    // InfoOverlayLayer reads.
    info_status::publish();

    // FR-16.4 / phase D.7 — flush any pending first-frame timing.
    // One-shot: cleared after the print so a steady scene with no
    // swaps stays quiet. Non-zero values prove the speculative
    // prepare() pass actually amortizes the swap; values close to
    // a steady-state frame budget mean the cache was warm.
    {
      const uint32_t ff = g_first_frame_render_ms;
      if (ff != 0u) {
        g_first_frame_render_ms = 0;
        Serial.print("[scene] first_frame_ms=");
        Serial.println(static_cast<unsigned long>(ff));
      }
    }

    // RTC poll cadence (1 h on accept, exponential backoff on
    // reject) — drives tod::poll_validated(). FR-9.5 / FR-13.5.
    rtc_poll::tick(now_ms);

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

    // IR receiver counters are surfaced live on-panel by the
    // IrTestScene (scene_id=ir_test, phase IR.1) — see
    // src/scenes/ir_test_scene.h. No serial mirror by design: the
    // panel readout is the diagnostic.
  }

  // Optional stress harnesses (CORE0_STRESS / CORE0_MQTT_FLOOD) — both
  // compile away when their macro isn't defined.
  stress_harness::tick(now_ms);

  // NFR-3.2 watchdog feed — only re-arms when Core 1's heartbeat is
  // fresh; a stall on either core trips a reset.
  watchdog::tick(now_ms, g_render_alive_ms);

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
  fm6126a::init();

  g_status = matrix.begin();

  if (g_status == PROTOMATTER_OK) {
    // Seed every background's deterministic state once. Cheap; no heap.
    g_backgrounds.init_all(matrix);

    compositor::init_default_scene(matrix);
    compositor::install_safety_overlays(matrix);
  }
}

void loop1() {
  if (g_status != PROTOMATTER_OK) return;
  compositor::tick(matrix, millis());
}
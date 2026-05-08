#include <Arduino.h>
#include <Adafruit_Protomatter.h>

#include "config.h"
#include "backgrounds.h"
#include "color_palette.h"
#include "ds3231.h"
#include "fixed_point.h"
#include "gfx_text.h"
#include "light_sensor.h"
#include "scene_state.h"
#include "sky_snapshot.h"
#include "theme.h"
#include "thermal_monitor.h"
#include "time_of_day.h"
#include "wifi_link.h"
#include "mqtt_link.h"
#include "iss_state.h"
#include "jupiter_state.h"
#include "constellation_state.h"
#include "moon_state.h"
#include "ir_remote.h"
#include "scenes/scene.h"
#include "scenes/layer.h"
#include "scenes/fade_black_layer.h"
#include "scenes/safety_overlay_layer.h"
#include "scenes/background_scene.h"
#include "scenes/boot_scene.h"
#include "scenes/clock_scene.h"
#include "scenes/color_cycle_scene.h"
#include "scenes/giant_clock_scene.h"
#include "scenes/gfx_test_scene.h"
#include "scenes/night_scene.h"
#include "scenes/offline_scene.h"
#include "scenes/iss_pass_scene.h"
#include "scenes/jupiter_visibility_scene.h"
#include "scenes/moon_phase_scene.h"
#include "scenes/constellation_now_scene.h"
#include "scenes/ir_test_scene.h"
#include "scenes/sky_timelapse_scene.h"
#include "scenes/splash_scene.h"
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
static BackgroundScene s_bg_bitmap  (BgType::BITMAP);
static BackgroundScene s_bg_image   (BgType::IMAGE);
[[maybe_unused]] static TextDemoScene s_text_demo_scene;
static GiantClockScene s_giant_clock_scene; // phase 3.5.3 — default room-clock view
static NightScene      s_night_scene;       // phase 5.5.1 — LDR-triggered override
static OfflineScene    s_offline_scene;     // phase 6.4 — MQTT-disconnect override
static SplashScene     s_splash_scene;      // phase 6.5+ — boot splash override
static ThermalSafeScene s_thermal_safe_scene; // phase 5.5.2 — DS3231-triggered override
static GfxTestScene    s_gfx_test_scene;    // graphics smoke-test (FPS, palette cycle)
static SkyTimelapseScene s_sky_timelapse_scene; // debug: 1 day per 10 s
static IssPassScene    s_iss_pass_scene;    // phase 7.1 — "ISS NOW" callout
static MoonPhaseScene  s_moon_phase_scene;  // phase 7.2 — sticky moon disc + phase
static JupiterVisibilityScene s_jupiter_visibility_scene; // phase 7.3 — Jupiter look-angles
static ConstellationNowScene  s_constellation_now_scene;  // phase 7.4 — dynamic constellation art
static IrTestScene            s_ir_test_scene;            // phase IR.1 — IR receiver POC readout

// Single "current scene" pointer; loop() just delegates to it. Swapping
// scenes is one assignment — no other code changes. (NFR-5.1)
static Scene* g_current_scene = &s_giant_clock_scene;

// ─── Compositor layer adapters (FR-16.1, phase D.1) ──────────────────
//
// loop1() now walks a fixed `Layer*` array each frame instead of calling
// g_current_scene->render() directly. For D.1 the only inhabitants are
// adapters around the existing scene + chrome path, so the visual output
// is unchanged. Future steps fill the empty overlay slots:
//   D.2 crossfade  → installs a CrossfadeLayer in OVERLAY_TRANSITION
//   D.3 overrides  → installs Night/Thermal/Offline/Splash overlays in
//                    OVERLAY_SAFETY (replacing today's preempting SceneIds)
//   D.8 toasts     → pushes ephemeral Layers into OVERLAY_TRANSITION
//
// Slot ordering is the compositor draw order (back-to-front).
enum LayerSlot : uint8_t {
  LAYER_FG = 0,           // active scene draws bg+fg here (legacy path)
  LAYER_OVERLAY_SAFETY,   // night / thermal / offline / splash (D.3)
  LAYER_OVERLAY_TRANSITION, // crossfades (D.2), toasts (D.8)
  LAYER_CHROME,           // shared HH:MM readout, future micro-indicators
  LAYER_COUNT
};

// Foreground adapter — delegates to whatever Scene g_current_scene points
// at. Lets the compositor treat the scene as just another Layer without
// every Scene having to inherit from Layer (NFR-5.1: adding a scene stays
// "registry entry + render function", no new base class).
class SceneFgLayer final : public Layer {
public:
  const char* name() const override {
    return g_current_scene ? g_current_scene->name() : "fg/none";
  }
  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (g_current_scene != nullptr) {
      g_current_scene->render(matrix, now_ms);
    }
  }
};

// Chrome adapter — the always-on HH:MM readout (FR-9.2). Honours the
// active scene's wants_clock_chrome() opt-out so the giant clock isn't
// defaced. Future D.8/D.9 work (link-health dot, sun arc) layers in
// here; keeping it as a Layer means those additions don't touch loop1().
class ChromeLayer final : public Layer {
public:
  const char* name() const override { return "chrome"; }
  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (g_current_scene != nullptr && g_current_scene->wants_clock_chrome()) {
      gfx::draw_clock_chrome(matrix, now_ms);
      // FR-16.5 micro-indicator: 1-px sun arc on the top edge,
      // driven by the sky_snapshot Core 1 publishes once per second.
      // Cheap (one snapshot read + one drawPixel) and shares the
      // wants_clock_chrome() opt-out so the giant clock isn't
      // defaced by stray top-row pixels.
      gfx::draw_sun_arc_chrome(matrix);
    }
  }
};

static SceneFgLayer s_layer_fg;
static ChromeLayer  s_layer_chrome;
static FadeBlackLayer s_fade_black_layer;  // D.2 scene transition (FR-16.3)
static SafetyOverlayLayer s_safety_overlay_layer;  // D.3 firmware overrides (FR-16.2)

// Pending swap target stashed when a fade starts. The actual
// g_current_scene swap is deferred to the fade midpoint so the panel
// is fully black during init(), masking any first-frame jitter.
static scene_state::SceneId s_fade_pending_id = scene_state::SceneId::CLOCK;

// FR-16.4 / phase D.7: armed by the post-swap branch in loop1() so
// the very next rendered frame logs its elapsed render-time. Lets us
// quantify the speculative-prepare win — pre-prepared scenes (e.g.
// ConstellationNow with its projection cache primed during the fade)
// should clock in close to steady-state, while a scene that bypasses
// prepare() still pays its first-frame setup cost. One-shot — cleared
// after the next [scene] first_frame_ms log line.
static volatile bool s_first_frame_pending = false;

// Compositor stack. Indexed by LayerSlot. Null entries are skipped.
// File-scope so D.2/D.3/D.8 can install/remove layers from setter
// functions without re-plumbing loop1().
static Layer* g_layers[LAYER_COUNT] = {
  &s_layer_fg,                 // LAYER_FG
  &s_safety_overlay_layer,     // LAYER_OVERLAY_SAFETY     (D.3)
  &s_fade_black_layer,         // LAYER_OVERLAY_TRANSITION (D.2 / D.8)
  &s_layer_chrome              // LAYER_CHROME
};

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
    case SI::BG_BITMAP:    return &s_bg_bitmap;
    case SI::BG_IMAGE:     return &s_bg_image;
    case SI::NIGHT:        return &s_night_scene;
    case SI::OFFLINE:      return &s_offline_scene;
    case SI::SPLASH:       return &s_splash_scene;
    case SI::THERMAL_SAFE: return &s_thermal_safe_scene;
    case SI::GFX_TEST:     return &s_gfx_test_scene;
    case SI::SKY_TIMELAPSE: return &s_sky_timelapse_scene;
    case SI::ISS_PASS:     return &s_iss_pass_scene;
    case SI::MOON_PHASE:   return &s_moon_phase_scene;
    case SI::JUPITER_VISIBILITY: return &s_jupiter_visibility_scene;
    case SI::CONSTELLATION_NOW:  return &s_constellation_now_scene;
    case SI::IR_TEST:            return &s_ir_test_scene;
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

// Core 1 → Core 0 FPS report. Written by loop1() once per second,
// read by loop() to print. Single naturally-aligned uint32_t write,
// reader tolerates a slightly stale value (it's a diagnostic), so no
// mutex needed. Keeping ALL Serial output on Core 0 avoids USB CDC
// interrupts disrupting Protomatter's PIO/DMA timing on Core 1 — the
// otherwise-unexplained "subtle once-per-second flicker".
//
// External linkage (NOT `static`) so mqtt_link.cpp can declare
// `extern volatile uint32_t g_render_fps` and surface this in the
// observatory/status heartbeat (§5.4).
volatile uint32_t g_render_fps = 0;

// Core 1 → Core 0 liveness heartbeat for the NFR-3.2 watchdog.
// loop1() writes millis() every iteration (cheap — even when frame-
// capped). loop() compares against now and only feeds the WDT when
// the heartbeat is fresh, so a Core 1 stall ≥ kRenderStallMs ends in
// a chip reset. Sentinel 0 = "Core 1 hasn't published yet"; Core 0
// keeps feeding the WDT during the boot window so the chip doesn't
// kill itself before setup1() runs. (added in phase 6.5)
static volatile uint32_t g_render_alive_ms = 0;

// Core 1 → Core 0 idle-slack telemetry (FR-16.9). Rolling average of
// `kFrameIntervalMs - actual_render_time` over the last 32 rendered
// frames, in milliseconds. Same atomic-uint32 pattern as g_render_fps:
// Core 1 publishes once per frame, Core 0 reads from the MQTT status
// builder. Quantifies how much headroom D.6 (sky-model) and D.7
// (Scene::prepare) have to play with on the current scene mix. A
// rising-with-load `render_slack_ms` falling toward zero is the
// canary for FR-3.1 violations before they're visible as dropped
// frames. Skipped frames (frame cap early-return) don't sample —
// only frames where matrix.show() actually ran. (added in phase D.5)
//
// External linkage so mqtt_link.cpp can declare
// `extern volatile uint32_t g_render_slack_ms` for the §5.4 heartbeat.
volatile uint32_t g_render_slack_ms = 0;

// Core 1 → Core 0 first-frame-after-swap render time (ms), one-shot.
// Set by loop1() the frame after a Scene swap; printed and cleared by
// loop() at its next 1 Hz log tick. Sentinel 0 = no fresh value.
// Diagnostic only — phase D.7 (FR-16.4) uses this to confirm the
// speculative prepare() actually amortizes the swap cost.
static volatile uint32_t g_first_frame_render_ms = 0;

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

  // Continuous sky-model snapshot (FR-16.5, phase D.6). Seeds an
  // explicitly-invalid snapshot so first-frame chrome readers don't
  // observe uninitialised seqlock state. The actual 1 Hz refresh
  // runs on Core 1 from loop1() during slack windows.
  sky_snapshot::init();

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
  rp2040.wdt_begin(8000);
  Serial.println("[wdt] enabled timeout=8000ms");

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

  // Phase 6.4: MQTT-disconnect override (FR-5.1). Edge-detect on
  // mqtt_link::connected() so we only wake the renderer when the
  // link state actually flips. set_offline_active() itself is
  // already a same-state no-op, but the explicit edge keeps the log
  // single-line per transition. The 1 s loop cadence puts us well
  // under FR-5.1's 5 s switch-to-offline budget.
  {
    static bool s_was_connected = false;
    static bool s_init_done     = false;
    static bool s_splash_cleared = false;
    const bool now_connected = mqtt_link::connected();
    if (!s_init_done || now_connected != s_was_connected) {
      scene_state::set_offline_active(!now_connected);
      if (s_init_done) {
        Serial.print("[mqtt] link ");
        Serial.println(now_connected ? "online" : "offline");
      }
      s_was_connected = now_connected;
      s_init_done     = true;
    }
    // Clear the boot splash on the first time MQTT comes up — the
    // device is now fully online and the operator-facing default
    // scene should take over. Latched: subsequent disconnects fall
    // through the normal offline override, not back to splash.
    if (now_connected && !s_splash_cleared) {
      scene_state::set_splash_active(false);
      s_splash_cleared = true;
      Serial.println("[splash] cleared (first mqtt connect)");
    }
  }

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

    // RTC poll cadence:
    //   - Default: every 1 hour. DS3231 drift is ~2 ppm (≈7 s/month),
    //     so the millis() projection in tod::now() is more than
    //     accurate enough between hourly resyncs.
    //   - Glitch handling: poll_validated() rejects readings that
    //     differ from the projected wall-clock by more than 3 hours
    //     (covers DST jumps, MQTT-driven set_from_mqtt corrections,
    //     and outright corruption). On reject we re-try with
    //     exponential backoff: 1 s → 2 s → 4 s → ... → 1 h, resetting
    //     to the 1 h cadence on the first accept.
    //   - First poll: scheduled immediately so chrome leaves "--:--"
    //     ASAP after boot.
    static uint32_t s_next_poll_at_ms = 0;
    static uint32_t s_backoff_ms      = 1000u;
    static constexpr uint32_t kPollOk_ms   = 60u * 60u * 1000u;  // 1 h
    static constexpr uint32_t kBackoffCap  = kPollOk_ms;
    static constexpr uint32_t kMaxJumpSec  = 3u * 60u * 60u;     // 3 h
    if (static_cast<int32_t>(now_ms - s_next_poll_at_ms) >= 0) {
      const bool accepted = tod::poll_validated(now_ms, kMaxJumpSec);
      if (accepted) {
        s_next_poll_at_ms = now_ms + kPollOk_ms;
        s_backoff_ms      = 1000u;
      } else {
        s_next_poll_at_ms = now_ms + s_backoff_ms;
        s_backoff_ms      = (s_backoff_ms >= kBackoffCap / 2u)
                              ? kBackoffCap
                              : (s_backoff_ms * 2u);
        Serial.print("[time] poll rejected, retry in ms=");
        Serial.println(static_cast<unsigned long>(s_backoff_ms));
      }
    }

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

  // Phase D.4 / FR-16.7 stress test: hammer scene_state::request() at
  // 20 Hz alternating between two scenes, simulating a Director (or
  // adversary) flooding observatory/scene. Validates that the seqlock
  // read path on Core 1 (take_pending / read_overrides) doesn't stall
  // on writer pressure \u2014 [render] fps=… should stay flat at the
  // kFrameIntervalMs cap. Off by default; enable with
  //   build_flags = -DCORE0_MQTT_FLOOD
#ifdef CORE0_MQTT_FLOOD
  {
    static uint32_t s_flood_last_ms = 0;
    static uint32_t s_flood_count = 0;
    static uint32_t s_flood_log_ms = 0;
    static bool     s_flood_toggle = false;
    if (now_ms - s_flood_last_ms >= 50u) {  // 20 Hz
      s_flood_last_ms = now_ms;
      const auto id = s_flood_toggle ? scene_state::SceneId::CLOCK
                                     : scene_state::SceneId::BG_NEBULA;
      s_flood_toggle = !s_flood_toggle;
      // priority 1 (default), short duration so tick() doesn't fight us.
      scene_state::request(id, 1, 30, false);
      ++s_flood_count;
    }
    if (now_ms - s_flood_log_ms >= 1000u) {
      s_flood_log_ms = now_ms;
      Serial.print("[flood] requests=");
      Serial.println(static_cast<unsigned long>(s_flood_count));
    }
  }
#endif

  // NFR-3.2 watchdog feed. Two states:
  //   (a) Boot window — Core 1 hasn't published a heartbeat yet
  //       (g_render_alive_ms == 0). Always feed so the WDT can't
  //       fire while setup1() is still running FM6126A init.
  //   (b) Steady state — only feed if Core 1's heartbeat is within
  //       kRenderStallMs. Past that, stop feeding and let the chip
  //       reset (~8 s timeout per rp2040.wdt_begin in setup()).
  // Core 0 itself is also covered: anything in this loop() that
  // blocks longer than the WDT timeout simply never reaches this
  // call → reset.
  {
    static constexpr uint32_t kRenderStallMs = 4000u;  // half the WDT timeout
    const uint32_t alive = g_render_alive_ms;
    if (alive == 0u || (now_ms - alive) < kRenderStallMs) {
      rp2040.wdt_reset();
    }
  }

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

  // D.3: bind safety override scenes to the overlay layer and run
  // their one-time init() (e.g. SplashScene resolves the observatory
  // bitmap from the asset registry here). They render on top of the
  // active fg scene whenever their flag is set; the dispatcher itself
  // never points g_current_scene at them anymore.
  if (g_status == PROTOMATTER_OK) {
    s_splash_scene.init(matrix);
    s_thermal_safe_scene.init(matrix);
    s_night_scene.init(matrix);
    s_offline_scene.init(matrix);
    s_safety_overlay_layer.bind(&s_splash_scene,
                                &s_thermal_safe_scene,
                                &s_night_scene,
                                &s_offline_scene);
  }
}

void loop1() {
  // FPS counter: count frames between wall-clock seconds and print once/sec.
  // Integer math only (NFR-1.3) — no float in the render loop.
  static uint32_t frames = 0;
  static uint32_t last_report_ms = 0;
  static uint32_t last_show_ms   = 0;

  // Frame pacing — fixes a "subtle flicker" caused by calling
  // matrix.show() as fast as the loop runs. RP2040 Protomatter swaps
  // the back/front buffer on the next bit-plane boundary; if show()
  // arrives at random offsets within the BCM refresh cycle, the
  // perceived per-pixel on-time jitters and the eye sees brightness
  // wobble. Capping at PANEL_TARGET_FPS_MS gives the panel a stable
  // cadence well within FR-3.1 (20–30 FPS target). Render-time slack
  // (we were running at hundreds of FPS) absorbs the cap with no
  // visible motion penalty.
  static constexpr uint32_t kFrameIntervalMs = 42;  // ~24 FPS (FR-3.1)

  const uint32_t now_ms = millis();

  // NFR-3.2: publish liveness on EVERY iteration, before the frame
  // cap can early-return. Core 0 reads this to decide whether to feed
  // the hardware watchdog. Single naturally-aligned 32-bit write —
  // atomic on RP2040, no mutex needed (same rationale as g_render_fps).
  g_render_alive_ms = now_ms;

  // Phase 4.2 / D.2: consume any pending scene change requested by
  // Core 0. With D.2 the swap is now gated by the fade-through-black
  // envelope: if the fade is in flight, we hold off on take_pending()
  // (so a queued request stays queued) and only actually swap
  // g_current_scene at the envelope's midpoint, when the panel is
  // fully black. Unknown ids leave the active scene alone (FR-1.3
  // spirit applied at the cross-core boundary).
  if (g_status == PROTOMATTER_OK) {
    if (s_fade_black_layer.active()) {
      // Mid-fade swap. ready_to_swap() returns true exactly once at
      // the envelope midpoint, so the init() runs under fully-black
      // panel and the new scene's first frame is invisible.
      if (s_fade_black_layer.ready_to_swap(now_ms)) {
        Scene* next = scene_for(s_fade_pending_id);
        if (next != nullptr && next != g_current_scene) {
          g_current_scene = next;
          g_current_scene->init(matrix);
          scene_state::mark_current(s_fade_pending_id);
          // FR-16.4 / phase D.7: arm a one-shot first-frame timer
          // so the next loop iteration can quantify the swap cost.
          // The pre-fade prepare() pass below should have warmed any
          // cache the incoming scene maintains; this confirms.
          s_first_frame_pending = true;
          Serial.print("[scene] swap -> ");
          Serial.println(g_current_scene->name());
        } else if (next == nullptr) {
          Serial.print("[scene] unknown id=");
          Serial.println(static_cast<int>(s_fade_pending_id));
        }
      } else {
        // FR-16.4 / phase D.7: speculative pre-render of the
        // incoming scene during the fade-out half. The hook is
        // idempotent — calling it every frame just re-checks the
        // scene's internal cache, which is microseconds. The cost
        // is bounded by Scene::prepare()'s contract (no draw, no
        // alloc, < ~5 ms one-shot work). For scenes that don't
        // override prepare() it's literally a virtual no-op call.
        Scene* incoming = scene_for(s_fade_pending_id);
        if (incoming != nullptr && incoming != g_current_scene) {
          incoming->prepare(now_ms);
        }
      }
    } else {
      scene_state::SceneId pending_id;
      if (scene_state::take_pending(&pending_id)) {
        Scene* next = scene_for(pending_id);
        if (next != nullptr && next != g_current_scene) {
          // Defer the actual swap to the fade midpoint. The fade
          // layer takes over LAYER_OVERLAY_TRANSITION until the
          // envelope completes (~250 ms).
          s_fade_pending_id = pending_id;
          s_fade_black_layer.start(now_ms);
        } else if (next == nullptr) {
          Serial.print("[scene] unknown id=");
          Serial.println(static_cast<int>(pending_id));
        }
        // Same-scene re-request (next == g_current_scene): silently
        // accepted by take_pending(); no fade, no init() rerun.
      }
    }
  }

  if (g_status == PROTOMATTER_OK && g_current_scene != nullptr) {
    // Frame cap: skip this iteration if we're ahead of schedule.
    // Wrap-safe (NFR §2 time math).
    if (static_cast<int32_t>(now_ms - last_show_ms) < static_cast<int32_t>(kFrameIntervalMs)) {
      return;
    }
    last_show_ms = now_ms;

    // Compositor walk (FR-16.1, phase D.1). Back-to-front, skipping
    // empty slots. The fg slot draws background+foreground (legacy
    // Scene contract); the chrome slot adds the always-on HH:MM
    // overlay (FR-9.2) honouring wants_clock_chrome(). Overlay slots
    // are unused in D.1 — D.2/D.3/D.8 will populate them.
    for (uint8_t i = 0; i < LAYER_COUNT; ++i) {
      if (g_layers[i] != nullptr) {
        g_layers[i]->render(matrix, now_ms);
      }
    }

    // Single show() per frame. (FR-9.3 — chrome must overlay before flip.)
    matrix.show();

    frames++;

    // Idle-slack instrumentation (FR-16.9, phase D.5). Sample only on
    // frames that actually rendered (frame-cap early-return doesn't
    // count — its "slack" is whatever's left of the cap, which is
    // already accounted for by the next iteration's wait). Use the
    // pre-show now_ms timestamp captured above as render_start;
    // millis() now is render_end. Clamp at 0 for over-budget frames so
    // the rolling average never goes negative when scenes occasionally
    // blow the cap. uint8_t is enough — kFrameIntervalMs is 42, well
    // under 256. The 32-frame ring + integer running sum (max
    // 32*kFrameIntervalMs = 1344, fits uint16_t) is cheaper than an
    // EMA divide and gives a flat-window average that's easy to
    // reason about: the published value lags load changes by ~32
    // frames (~1.3 s at 24 FPS), which is the right scale for HA's
    // 30 s heartbeat consumer.
    {
      const uint32_t render_ms = millis() - now_ms;
      const uint8_t  sample    = (render_ms >= kFrameIntervalMs)
                                   ? 0
                                   : static_cast<uint8_t>(kFrameIntervalMs - render_ms);
      static uint8_t  s_slack_ring[32] = {0};
      static uint16_t s_slack_sum      = 0;
      static uint8_t  s_slack_idx      = 0;
      s_slack_sum -= s_slack_ring[s_slack_idx];
      s_slack_ring[s_slack_idx] = sample;
      s_slack_sum += sample;
      s_slack_idx = (s_slack_idx + 1u) & 31u;
      // Single naturally-aligned 32-bit store — atomic on RP2040, no
      // mutex needed. Reader (mqtt_link::publish_status) tolerates a
      // slightly stale value; it's a diagnostic, not a control input.
      g_render_slack_ms = static_cast<uint32_t>(s_slack_sum >> 5);  // /32

      // FR-16.4 / phase D.7: one-shot first-frame timing. Capture
      // the same render_ms we just sampled and hand it to Core 0
      // for logging — Serial from Core 1 would race Protomatter
      // PIO/DMA timing (CODING_PRACTICES §10). Sentinel 0 means
      // "nothing new"; pin to >=1 so a literal sub-ms render still
      // logs.
      if (s_first_frame_pending) {
        s_first_frame_pending = false;
        const uint32_t v = (render_ms == 0u) ? 1u : render_ms;
        g_first_frame_render_ms = v;
      }
    }

    // FR-16.5 / phase D.6: continuous sky-model refresh on Core 1.
    // Internally rate-limited to ~1 Hz and gated on the freshly-
    // computed slack so it skips frames where the active scene is
    // already at budget. The trig-heavy sun::compute + iss_geom
    // cost (~200 µs combined on RP2040 soft-float) lands here once a
    // second on a slack window — moves it off Core 0 entirely and
    // off the per-frame render path that scenes currently run.
    sky_snapshot::tick(now_ms, g_render_slack_ms);
  }

  // Publish FPS to Core 0 once per second WITHOUT printing here —
  // Serial output on Core 1 contends with Protomatter's PIO/DMA timing
  // and produces a once-per-second flicker. Core 0's loop() reads
  // g_render_fps and logs it instead.
  if (now_ms - last_report_ms >= 1000u) {
    g_render_fps = frames;
    frames = 0;
    last_report_ms = now_ms;
  }
}
#include "compositor.h"

#include <Arduino.h>

#include "config.h"
#include "gfx_text.h"
#include "scene_registry.h"
#include "scene_state.h"
#include "scenes/fade_black_layer.h"
#include "scenes/info_overlay_layer.h"
#include "scenes/layer.h"
#include "scenes/safety_overlay_layer.h"
#include "scenes/scene.h"
#include "scenes/settings_overlay_layer.h"
#include "input/ir_actions.h"

// ─── Cross-core render telemetry definitions ────────────────────────
volatile uint32_t g_render_fps             = 0;
volatile uint32_t g_render_alive_ms        = 0;
volatile uint32_t g_render_slack_ms        = 0;
volatile uint32_t g_first_frame_render_ms  = 0;

namespace {

// Active scene pointer; loop1 (via compositor::tick) renders through
// the SceneFgLayer adapter that delegates here.
Scene* g_current_scene = scene_registry::default_scene();

// ─── Compositor layer adapters (FR-16.1, phase D.1) ────────────────
// Slot ordering is the compositor draw order (back-to-front).
enum LayerSlot : uint8_t {
  LAYER_FG = 0,            // active scene draws bg+fg here (legacy path)
  LAYER_OVERLAY_SAFETY,    // night / thermal / offline / splash (D.3)
  LAYER_OVERLAY_SETTINGS,  // FR-19 settings menu (above SAFETY — operator
                           // can adjust during NIGHT / THERMAL_SAFE; above
                           // INFO since the menu is an active foreground UI)
  LAYER_OVERLAY_INFO,      // operator-triggered diagnostic overlay (IR.4)
  LAYER_OVERLAY_TRANSITION,// crossfades (D.2), toasts (D.8)
  LAYER_CHROME,            // shared HH:MM readout, future micro-indicators
  LAYER_COUNT
};

// Foreground adapter — delegates to whatever Scene g_current_scene
// points at. Lets the compositor treat the scene as just another
// Layer without every Scene having to inherit from Layer (NFR-5.1:
// adding a scene stays "registry entry + render function", no new
// base class).
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

// Forward-declared first so ChromeLayer::render() can query its
// covering_override() — the actual instance lives below.
SafetyOverlayLayer s_safety_overlay_layer;  // D.3 firmware overrides (FR-16.2)

// Chrome adapter — the always-on HH:MM readout (FR-9.2). Honours the
// active scene's wants_clock_chrome() opt-out so the giant clock isn't
// defaced; when a safety override is fully covering the panel, the
// override's hints win over the (invisible) underlying scene's, so
// NIGHT / OFFLINE / THERMAL / SPLASH can suppress the corner readout
// even when the bg scene wanted it. Future link-health dot work
// layers in here; keeping it as a Layer means those additions don't
// touch loop1().
//
// Without this, NIGHT's `wants_clock_chrome() == false` was ignored
// whenever the underlying scene happened to be a chrome-on one
// (clock_scene, iss, jupiter, moon, …), causing the small white
// HH:MM in the top-right corner to bleed through the deep-red night
// field. Same applies to Blade Runner's cyan FRAME_BORDER painting
// over NIGHT.
class ChromeLayer final : public Layer {
public:
  const char* name() const override { return "chrome"; }
  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    Scene* hint_scene = s_safety_overlay_layer.covering_override();
    if (hint_scene == nullptr) hint_scene = g_current_scene;

    if (hint_scene != nullptr && hint_scene->wants_clock_chrome()) {
      gfx::draw_clock_chrome(matrix, now_ms);
    }
    if (hint_scene == nullptr || hint_scene->wants_theme_decorations()) {
      gfx::draw_theme_decorations(matrix);
    }
  }
};

SceneFgLayer     s_layer_fg;
ChromeLayer      s_layer_chrome;
FadeBlackLayer   s_fade_black_layer;    // D.2 scene transition (FR-16.3)
InfoOverlayLayer s_info_overlay_layer;  // IR.4 operator diagnostic overlay (FR-17.8)
SettingsOverlayLayer s_settings_overlay_layer;  // FR-19 settings menu

// Pending swap target stashed when a fade starts. The actual
// g_current_scene swap is deferred to the fade midpoint so the panel
// is fully black during init(), masking any first-frame jitter.
scene_state::SceneId s_fade_pending_id = scene_state::SceneId::CLOCK;

// FR-16.4 / phase D.7: armed by the post-swap branch so the very
// next rendered frame logs its elapsed render-time. One-shot —
// cleared after the next [scene] first_frame_ms log line.
volatile bool s_first_frame_pending = false;

// Compositor stack. Indexed by LayerSlot. Null entries are skipped.
// File-scope so D.2/D.3/D.8 can install/remove layers from setter
// functions without re-plumbing tick().
Layer* g_layers[LAYER_COUNT] = {
  &s_layer_fg,                 // LAYER_FG
  &s_safety_overlay_layer,     // LAYER_OVERLAY_SAFETY     (D.3)
  &s_settings_overlay_layer,   // LAYER_OVERLAY_SETTINGS   (FR-19)
  &s_info_overlay_layer,       // LAYER_OVERLAY_INFO       (IR.4)
  &s_fade_black_layer,         // LAYER_OVERLAY_TRANSITION (D.2 / D.8)
  &s_layer_chrome              // LAYER_CHROME
};

}  // namespace

namespace compositor {

void init_default_scene(Adafruit_Protomatter& matrix) {
  if (g_current_scene == nullptr) return;
  g_current_scene->init(matrix);
  Serial.print("[scene] active=");
  Serial.println(g_current_scene->name());
}

void install_safety_overlays(Adafruit_Protomatter& matrix) {
  using SI = scene_state::SceneId;
  Scene* splash  = scene_registry::scene_for(SI::SPLASH);
  Scene* thermal = scene_registry::scene_for(SI::THERMAL_SAFE);
  Scene* night   = scene_registry::scene_for(SI::NIGHT);
  Scene* offline = scene_registry::scene_for(SI::OFFLINE);
  splash->init(matrix);
  thermal->init(matrix);
  night->init(matrix);
  offline->init(matrix);
  s_safety_overlay_layer.bind(splash, thermal, night, offline);
}

void tick(Adafruit_Protomatter& matrix, uint32_t now_ms) {
  // Frame pacing — fixes a "subtle flicker" caused by calling
  // matrix.show() as fast as the loop runs. RP2040 Protomatter swaps
  // the back/front buffer on the next bit-plane boundary; if show()
  // arrives at random offsets within the BCM refresh cycle, the
  // perceived per-pixel on-time jitters and the eye sees brightness
  // wobble. Capping at PANEL_TARGET_FPS_MS gives the panel a stable
  // cadence well within FR-3.1 (20–30 FPS target).
  static constexpr uint32_t kFrameIntervalMs = 42;  // ~24 FPS (FR-3.1)

  static uint32_t frames         = 0;
  static uint32_t last_report_ms = 0;
  static uint32_t last_show_ms   = 0;

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
  if (s_fade_black_layer.active()) {
    // Mid-fade swap. ready_to_swap() returns true exactly once at
    // the envelope midpoint, so the init() runs under fully-black
    // panel and the new scene's first frame is invisible.
    if (s_fade_black_layer.ready_to_swap(now_ms)) {
      Scene* next = scene_registry::scene_for(s_fade_pending_id);
      if (next != nullptr && next != g_current_scene) {
        g_current_scene = next;
        g_current_scene->init(matrix);
        scene_state::mark_current(s_fade_pending_id);
        // FR-16.4 / phase D.7: arm a one-shot first-frame timer so
        // the next loop iteration can quantify the swap cost. The
        // pre-fade prepare() pass below should have warmed any cache
        // the incoming scene maintains; this confirms.
        s_first_frame_pending = true;
        Serial.print("[scene] swap -> ");
        Serial.println(g_current_scene->name());
      } else if (next == nullptr) {
        Serial.print("[scene] unknown id=");
        Serial.println(static_cast<int>(s_fade_pending_id));
      }
    } else {
      // FR-16.4 / phase D.7: speculative pre-render of the incoming
      // scene during the fade-out half. The hook is idempotent —
      // calling it every frame just re-checks the scene's internal
      // cache, which is microseconds. The cost is bounded by
      // Scene::prepare()'s contract (no draw, no alloc, < ~5 ms
      // one-shot work). For scenes that don't override prepare()
      // it's literally a virtual no-op call.
      Scene* incoming = scene_registry::scene_for(s_fade_pending_id);
      if (incoming != nullptr && incoming != g_current_scene) {
        incoming->prepare(now_ms);
      }
    }
  } else {
    scene_state::SceneId pending_id;
    if (scene_state::take_pending(&pending_id)) {
      Scene* next = scene_registry::scene_for(pending_id);
      if (next != nullptr && next != g_current_scene) {
        // Defer the actual swap to the fade midpoint. The fade layer
        // takes over LAYER_OVERLAY_TRANSITION until the envelope
        // completes (~250 ms).
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

  if (g_current_scene == nullptr) return;

  // Scene-focus key dispatch (cross-core from ir_actions.cpp on Core 0).
  // Edge-detect by counter change so a key that arrives exactly once is
  // delivered exactly once, and a stale event value (e.g. the boot 0
  // sentinel) is never dispatched. Placed AFTER the swap branch so a
  // press in the same frame as a swap is delivered to the NEW scene
  // (acceptable — the user pressed OK to focus a scene, and any
  // navigation key should hit the now-visible scene). Cheap when idle:
  // one volatile read + compare.
  {
    static uint32_t s_last_key_event = 0;
    const uint32_t cur = g_scene_key_event;
    if (cur != 0u && cur != s_last_key_event) {
      s_last_key_event = cur;
      const SceneKey key = static_cast<SceneKey>(cur & 0xFFu);
      g_current_scene->on_key(key, now_ms);
    }
  }

  // Frame cap: skip this iteration if we're ahead of schedule.
  // Wrap-safe (NFR §2 time math).
  if (static_cast<int32_t>(now_ms - last_show_ms) <
      static_cast<int32_t>(kFrameIntervalMs)) {
    return;
  }
  last_show_ms = now_ms;

  // Compositor walk (FR-16.1). Back-to-front, skipping empty slots.
  // The fg slot draws background+foreground (legacy Scene contract);
  // the chrome slot adds the always-on HH:MM overlay (FR-9.2)
  // honouring wants_clock_chrome().
  for (uint8_t i = 0; i < LAYER_COUNT; ++i) {
    if (g_layers[i] != nullptr) {
      g_layers[i]->render(matrix, now_ms);
    }
  }

  // Single show() per frame. (FR-9.3 — chrome must overlay before flip.)
  matrix.show();

  frames++;

  // Idle-slack instrumentation (FR-16.9, phase D.5). Sample only on
  // frames that actually rendered. Use the pre-show now_ms timestamp
  // captured above as render_start; millis() now is render_end.
  // Clamp at 0 for over-budget frames so the rolling average never
  // goes negative when scenes occasionally blow the cap. The 32-frame
  // ring + integer running sum (max 32*kFrameIntervalMs = 1344, fits
  // uint16_t) is cheaper than an EMA divide and gives a flat-window
  // average that's easy to reason about: the published value lags
  // load changes by ~32 frames (~1.3 s at 24 FPS), the right scale
  // for HA's 30 s heartbeat consumer.
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
    g_render_slack_ms = static_cast<uint32_t>(s_slack_sum >> 5);  // /32

    // FR-16.4 / phase D.7: one-shot first-frame timing. Capture the
    // same render_ms we just sampled and hand it to Core 0 for
    // logging — Serial from Core 1 would race Protomatter PIO/DMA
    // timing (CODING_PRACTICES §10). Sentinel 0 means "nothing new";
    // pin to >=1 so a literal sub-ms render still logs.
    if (s_first_frame_pending) {
      s_first_frame_pending = false;
      const uint32_t v = (render_ms == 0u) ? 1u : render_ms;
      g_first_frame_render_ms = v;
    }
  }

  // Publish FPS to Core 0 once per second WITHOUT printing here —
  // Serial output on Core 1 contends with Protomatter's PIO/DMA
  // timing and produces a once-per-second flicker. Core 0's loop()
  // reads g_render_fps and logs it instead.
  if (now_ms - last_report_ms >= 1000u) {
    g_render_fps = frames;
    frames = 0;
    last_report_ms = now_ms;
  }
}

}  // namespace compositor

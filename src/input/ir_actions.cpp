#include "ir_actions.h"

#include <Arduino.h>

#include "buzzer.h"
#include "config.h"
#include "ir_remote.h"
#include "prefs.h"
#include "scene_state.h"
#include "scenes/font_demo_scene.h"
#include "scenes/scene.h"
#include "settings_ui.h"
#include "theme.h"

namespace ir_actions {

namespace {

constexpr uint8_t  kRemoteCyclePriority  = 1;     // FR-17.6
constexpr uint16_t kRemoteCycleDurationS = 120;   // FR-17.6

// Scene-focus duration. Long enough that the operator can navigate
// inside a scene without the underlying duration timer yanking them
// back to CLOCK mid-flow; the FR-2.4 hard 1 h TTL is still the hard
// ceiling, and BACK / HOME exit at any time.
constexpr uint16_t kFocusDurationS = 600;         // 10 min

FontDemoScene* s_font_demo_scene = nullptr;

// ─── Scene-focus state (FR-19+/intra-scene nav) ─────────────────────
// All scene-focus state is Core-0-only — IR dispatch runs single-
// threaded on Core 0, and the reconciler tick() also runs there. No
// cross-core safety needed beyond the g_scene_key_event event word
// which is consumed by Core 1 (see ir_actions.h).
bool s_focused = false;
// Remembered active-scene id at the moment of focus. The reconciler
// tick() compares against scene_state::current() each loop; a mismatch
// means an external party (MQTT Director, ISS auto-switch, sticky
// expiry, firmware override) replaced the focused scene out from under
// us, and focus must release so global ▲/▼/◄/► resume working.
scene_state::SceneId s_focused_scene_id = scene_state::SceneId::CLOCK;

// Packed event word: low 8 bits = SceneKey, high 24 bits = monotonic
// counter. Counter always increments so a same-key repeat still flips
// the word (Core 1 edge-detects by inequality). Pre-incremented before
// the OR so the sentinel 0 ("no event ever fired") is never reachable
// from a real publish.
uint32_t s_key_counter = 0;
void publish_scene_key(SceneKey key) {
  ++s_key_counter;
  if ((s_key_counter & 0x00FFFFFFu) == 0u) s_key_counter = 1u;  // skip 0
  g_scene_key_event = (s_key_counter << 8) | static_cast<uint8_t>(key);
}

// Distinctive two-note cues. Ascending = "now locked in" (enter),
// descending = "released" (exit). Frequencies sit between the
// theme-melody octaves and the 9 kHz IR-ack chirp so they read as a
// separate UX gesture. ~50 ms per note → ~100 ms total, well under
// the next-press interval. Boot/night quiet gates (FR-10.8 / FR-10.9)
// suppress these automatically at the driver boundary.
constexpr buzzer::Note kFocusEnterNotes[] = {
  {3200, 45},
  {4800, 60},
};
constexpr buzzer::Note kFocusExitNotes[] = {
  {4800, 45},
  {3200, 60},
};

void enter_focus() {
  if (s_focused) return;
  s_focused = true;
  s_focused_scene_id = scene_state::current();
  // Re-request the same scene as sticky so the FR-2.3 duration timer
  // doesn't yank it out from under the user mid-navigation. user_intent
  // ensures we win over any firmware-initiated sticky still active
  // (same discipline as the existing cycle path). duration_s is ignored
  // by scene_state when sticky=true but pass a sensible value anyway
  // for consistency.
  scene_state::request(s_focused_scene_id,
                       kRemoteCyclePriority,
                       kFocusDurationS,
                       /*sticky=*/true,
                       /*user_intent=*/true);
  if (prefs::current().button_sound) {
    buzzer::play(kFocusEnterNotes,
                 sizeof(kFocusEnterNotes) / sizeof(kFocusEnterNotes[0]));
  }
  Serial.print("[ir] focus enter scene=");
  Serial.println(scene_state::string_from_id(s_focused_scene_id));
}

void exit_focus() {
  if (!s_focused) return;
  s_focused = false;
  // Don't touch scene_state here — back() / home() / the reconciler
  // each apply their own follow-up (clear_sticky + request CLOCK, or
  // nothing at all when an external swap already happened). Keeps
  // this helper a pure state-flip + cue.
  if (prefs::current().button_sound) {
    buzzer::play(kFocusExitNotes,
                 sizeof(kFocusExitNotes) / sizeof(kFocusExitNotes[0]));
  }
  Serial.println("[ir] focus exit");
}

uint8_t remote_cycle_index_of_current() {
  const scene_state::SceneId cur = scene_state::current();
  for (uint8_t i = 0; i < kRemoteCycleCount; ++i) {
    if (kRemoteCycle[i] == cur) return i;
  }
  return kRemoteCycleCount;
}

// Common backbone for ▲ (delta=+1) and ▼ (delta=-1). When the active
// scene isn't in the cycle list, ▲ lands on entry 0 and ▼ on the last
// entry — feels natural because the operator's mental model is "step
// into the curated list".
void remote_cycle_step(int8_t delta) {
  const uint8_t cur = remote_cycle_index_of_current();
  uint8_t next;
  if (cur == kRemoteCycleCount) {
    next = (delta > 0) ? 0u : (kRemoteCycleCount - 1u);
  } else {
    next = static_cast<uint8_t>(
        (cur + kRemoteCycleCount + (delta > 0 ? 1 : -1)) % kRemoteCycleCount);
  }
  scene_state::request(kRemoteCycle[next],
                       kRemoteCyclePriority,
                       kRemoteCycleDurationS,
                       /*sticky=*/false,
                       /*user_intent=*/true);  // IR press always wins
}

void scene_next(uint16_t /*addr*/, uint16_t /*cmd*/)  {
  // FR-19 carve-out: when the settings menu is up, ▲ navigates
  // category/items rather than cycling scenes underneath. The menu
  // always wins over scene-focus.
  if (settings_ui::is_open()) { settings_ui::nav_up();   return; }
  if (s_focused) { publish_scene_key(SceneKey::UP); return; }
  remote_cycle_step(+1);
}
void scene_prev(uint16_t /*addr*/, uint16_t /*cmd*/)  {
  if (settings_ui::is_open()) { settings_ui::nav_down(); return; }
  if (s_focused) { publish_scene_key(SceneKey::DOWN); return; }
  remote_cycle_step(-1);
}

// FR-17.5 Back: clear any sticky scene + return to default CLOCK; also
// dismiss the boot splash if it's still latched. In scene-focus mode
// BACK is the one-step exit — release focus AND fall through to the
// global return-to-CLOCK path (the focus cue + the scene change cue
// stack via buzzer latest-wins, which sounds correct: a quick "tweep"
// then the home scene).
void back(uint16_t /*addr*/, uint16_t /*cmd*/) {
  // FR-19 carve-out: BACK inside the menu pops out of CATEGORY →
  // ROOT, then ROOT → close. Only fall through to scene-state
  // clear when the menu isn't up.
  if (settings_ui::is_open()) { settings_ui::back(); return; }
  if (s_focused) exit_focus();
  scene_state::set_splash_active(false);
  scene_state::clear_sticky();
  scene_state::request(scene_state::SceneId::CLOCK,
                       kRemoteCyclePriority,
                       kRemoteCycleDurationS,
                       /*sticky=*/false,
                       /*user_intent=*/true);
}

// FR-17.5 Home: jump to default CLOCK without clearing sticky state.
// FR-19 — also force-closes the settings menu so the operator
// always lands at a known state ("go home" is the panic button).
// Releases scene-focus too — the operator just asked to leave.
void home(uint16_t /*addr*/, uint16_t /*cmd*/) {
  if (settings_ui::is_open()) settings_ui::force_close();
  if (s_focused) exit_focus();
  scene_state::request(scene_state::SceneId::CLOCK,
                       kRemoteCyclePriority,
                       kRemoteCycleDurationS,
                       /*sticky=*/false,
                       /*user_intent=*/true);
}

// FR-17.8 / IR.4 — toggle the diagnostic info overlay.
// Bound to the on-board MENU button (GP15) per FR-19 — the IR
// remote OK button is now used to commit settings inside the
// FR-19 overlay (see ok() below) and is silent when the menu is
// closed (the FR-10.6 ack chirp upstream still confirms the
// press took). Kept as a free function with the original signature
// so the on-board button driver can publish to the same
// g_info_overlay_event_ms primitive without re-implementing it.
void info_toggle(uint16_t /*addr*/, uint16_t /*cmd*/) {
  uint32_t ts = millis();
  if (ts == 0u) ts = 1u;  // avoid the "no event" sentinel
  g_info_overlay_event_ms = ts;
}

// FR-19 — OK action. Priority order (highest first):
//   1. Settings menu open → commit_ok (menu ALWAYS wins).
//   2. Scene already focused → forward as SceneKey::OK so the scene
//      can advance its own info panels / drill-down state.
//   3. Otherwise → enter scene-focus mode (the scene under the cursor
//      becomes sticky + captures ▲▼◄►). The FR-10.6 ack chirp upstream
//      still fires first; the latest-wins buzzer scheduler then plays
//      the focus-enter cue on top.
void ok(uint16_t /*addr*/, uint16_t /*cmd*/) {
  if (settings_ui::is_open()) { settings_ui::commit_ok(); return; }
  if (s_focused) { publish_scene_key(SceneKey::OK); return; }
  enter_focus();
}

// FR-19 — `*` (Options) toggles the settings overlay open/close.
// Scene-focus persists across menu open/close — the operator can
// adjust settings without losing their place inside a focused scene.
void options(uint16_t /*addr*/, uint16_t /*cmd*/) {
  settings_ui::toggle_open();
}

// LEFT / RIGHT — theme cycle (FR-17.10) with FONT_DEMO carveout that
// routes to FontDemoScene's font picker so the diagnostic stays
// usable from the couch. In scene-focus the keys are forwarded to
// the scene instead; the FONT_DEMO carve-out is preserved so the
// existing diagnostic UX doesn't require an OK-to-focus first.
void left(uint16_t /*addr*/, uint16_t /*cmd*/) {
  if (settings_ui::is_open()) { settings_ui::nav_left(); return; }
  if (s_focused) { publish_scene_key(SceneKey::LEFT); return; }
  if (s_font_demo_scene != nullptr &&
      scene_state::current() == scene_state::SceneId::FONT_DEMO) {
    s_font_demo_scene->cycle(-1);
    return;
  }
  // FR-17.10 / FR-18.3 — route through prefs so the choice persists
  // (P.4). prefs::cycle_theme() drives theme::set() internally, so
  // FR-15.4 next-frame swap semantics are preserved.
  prefs::cycle_theme(-1);
}
void right(uint16_t /*addr*/, uint16_t /*cmd*/) {
  if (settings_ui::is_open()) { settings_ui::nav_right(); return; }
  if (s_focused) { publish_scene_key(SceneKey::RIGHT); return; }
  if (s_font_demo_scene != nullptr &&
      scene_state::current() == scene_state::SceneId::FONT_DEMO) {
    s_font_demo_scene->cycle(+1);
    return;
  }
  prefs::cycle_theme(+1);
}

// FR-17.5 dispatch table. `honour_repeats=false` everywhere because
// every action is discrete (FR-17.4 — long-press must not stampede).
constexpr ir_remote::DispatchEntry kIrDispatch[] = {
    {kIrButtonUpCmd,      ir_remote::Lane::LOCAL, false, &scene_next, "up"},
    {kIrButtonDownCmd,    ir_remote::Lane::LOCAL, false, &scene_prev, "down"},
    {kIrButtonOkCmd,      ir_remote::Lane::LOCAL, false, &ok,         "ok"},
    {kIrButtonBackCmd,    ir_remote::Lane::LOCAL, false, &back,       "back"},
    {kIrButtonHomeCmd,    ir_remote::Lane::LOCAL, false, &home,       "home"},
    {kIrButtonLeftCmd,    ir_remote::Lane::LOCAL, false, &left,       "left"},
    {kIrButtonRightCmd,   ir_remote::Lane::LOCAL, false, &right,      "right"},
    {kIrButtonOptionsCmd, ir_remote::Lane::LOCAL, false, &options,    "options"},
};
constexpr uint8_t kIrDispatchCount = sizeof(kIrDispatch) / sizeof(kIrDispatch[0]);

}  // namespace

void install_dispatch_table(FontDemoScene* font_demo) {
  s_font_demo_scene = font_demo;
  ir_remote::set_dispatch(kIrDispatch, kIrDispatchCount,
                          IR_REMOTE_ADDR_EXPECTED);
}

// Core-0 reconciler. Self-heal scene-focus when an external party
// (MQTT Director, ISS auto-switch, sticky expiry via scene_state::tick,
// firmware override) replaces the focused scene without going through
// ir_actions. Without this, ▲ on a "ghost-focused" scene would still
// swallow the press and the operator would be stuck.
//
// Note: firmware overrides (NIGHT / THERMAL_SAFE / OFFLINE / SPLASH)
// layer as overlays without changing scene_state::current() (see
// scene_state.h "override-as-overlay model"), so they intentionally
// do NOT release focus — the underlying scene is still the active
// Director scene and resumes visibly when the override clears.
void tick(uint32_t /*now_ms*/) {
  if (!s_focused) return;
  if (scene_state::current() != s_focused_scene_id) {
    exit_focus();
  }
}

}  // namespace ir_actions

// Cross-core IR.4 trigger definition (extern declared in ir_actions.h
// and consumed by InfoOverlayLayer on Core 1).
volatile uint32_t g_info_overlay_event_ms = 0;

// Cross-core scene-focus key event (extern declared in ir_actions.h
// and consumed by the compositor on Core 1).
volatile uint32_t g_scene_key_event = 0;

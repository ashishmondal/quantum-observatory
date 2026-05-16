#include "ir_actions.h"

#include <Arduino.h>

#include "config.h"
#include "ir_remote.h"
#include "prefs.h"
#include "scene_state.h"
#include "scenes/font_demo_scene.h"
#include "settings_ui.h"
#include "theme.h"

namespace ir_actions {

namespace {

constexpr uint8_t  kRemoteCyclePriority  = 1;     // FR-17.6
constexpr uint16_t kRemoteCycleDurationS = 120;   // FR-17.6

FontDemoScene* s_font_demo_scene = nullptr;

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
  // category/items rather than cycling scenes underneath.
  if (settings_ui::is_open()) { settings_ui::nav_up();   return; }
  remote_cycle_step(+1);
}
void scene_prev(uint16_t /*addr*/, uint16_t /*cmd*/)  {
  if (settings_ui::is_open()) { settings_ui::nav_down(); return; }
  remote_cycle_step(-1);
}

// FR-17.5 Back: clear any sticky scene + return to default CLOCK; also
// dismiss the boot splash if it's still latched.
void back(uint16_t /*addr*/, uint16_t /*cmd*/) {
  // FR-19 carve-out: BACK inside the menu pops out of CATEGORY →
  // ROOT, then ROOT → close. Only fall through to scene-state
  // clear when the menu isn't up.
  if (settings_ui::is_open()) { settings_ui::back(); return; }
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
void home(uint16_t /*addr*/, uint16_t /*cmd*/) {
  if (settings_ui::is_open()) settings_ui::force_close();
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

// FR-19 — OK action. Inside the settings menu OK enters a
// category (ROOT) or commits a value (CATEGORY); outside the menu
// OK is intentionally a no-op (the FR-10.6 chirp upstream is the
// only feedback). Prior IR.4 binding (OK → info overlay toggle)
// has moved to the on-board MENU button so OK can serve as the
// menu commit key without ambiguity.
void ok(uint16_t /*addr*/, uint16_t /*cmd*/) {
  if (settings_ui::is_open()) settings_ui::commit_ok();
}

// FR-19 — `*` (Options) toggles the settings overlay open/close.
void options(uint16_t /*addr*/, uint16_t /*cmd*/) {
  settings_ui::toggle_open();
}

// LEFT / RIGHT — theme cycle (FR-17.10) with FONT_DEMO carveout that
// routes to FontDemoScene's font picker so the diagnostic stays
// usable from the couch.
void left(uint16_t /*addr*/, uint16_t /*cmd*/) {
  // FR-19 carve-out: ◄ inside settings nudges the value down
  // (DISPLAY tint -10 %, SOUND tick mode prev). Theme cycle resumes
  // when the menu is closed.
  if (settings_ui::is_open()) { settings_ui::nav_left(); return; }
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

}  // namespace ir_actions

// Cross-core IR.4 trigger definition (extern declared in ir_actions.h
// and consumed by InfoOverlayLayer on Core 1).
volatile uint32_t g_info_overlay_event_ms = 0;

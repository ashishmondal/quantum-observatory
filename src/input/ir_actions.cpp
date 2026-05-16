#include "ir_actions.h"

#include <Arduino.h>

#include "config.h"
#include "ir_remote.h"
#include "prefs.h"
#include "scene_state.h"
#include "scenes/font_demo_scene.h"
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

void scene_next(uint16_t /*addr*/, uint16_t /*cmd*/)  { remote_cycle_step(+1); }
void scene_prev(uint16_t /*addr*/, uint16_t /*cmd*/)  { remote_cycle_step(-1); }

// FR-17.5 Back: clear any sticky scene + return to default CLOCK; also
// dismiss the boot splash if it's still latched.
void back(uint16_t /*addr*/, uint16_t /*cmd*/) {
  scene_state::set_splash_active(false);
  scene_state::clear_sticky();
  scene_state::request(scene_state::SceneId::CLOCK,
                       kRemoteCyclePriority,
                       kRemoteCycleDurationS,
                       /*sticky=*/false,
                       /*user_intent=*/true);
}

// FR-17.5 Home: jump to default CLOCK without clearing sticky state.
void home(uint16_t /*addr*/, uint16_t /*cmd*/) {
  scene_state::request(scene_state::SceneId::CLOCK,
                       kRemoteCyclePriority,
                       kRemoteCycleDurationS,
                       /*sticky=*/false,
                       /*user_intent=*/true);
}

// FR-17.8 / IR.4 — toggle the diagnostic info overlay.
void info_toggle(uint16_t /*addr*/, uint16_t /*cmd*/) {
  uint32_t ts = millis();
  if (ts == 0u) ts = 1u;  // avoid the "no event" sentinel
  g_info_overlay_event_ms = ts;
}

// LEFT / RIGHT — theme cycle (FR-17.10) with FONT_DEMO carveout that
// routes to FontDemoScene's font picker so the diagnostic stays
// usable from the couch.
void left(uint16_t /*addr*/, uint16_t /*cmd*/) {
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
    {kIrButtonUpCmd,    ir_remote::Lane::LOCAL, false, &scene_next,   "up"},
    {kIrButtonDownCmd,  ir_remote::Lane::LOCAL, false, &scene_prev,   "down"},
    {kIrButtonOkCmd,    ir_remote::Lane::LOCAL, false, &info_toggle,  "ok"},
    {kIrButtonBackCmd,  ir_remote::Lane::LOCAL, false, &back,         "back"},
    {kIrButtonHomeCmd,  ir_remote::Lane::LOCAL, false, &home,         "home"},
    {kIrButtonLeftCmd,  ir_remote::Lane::LOCAL, false, &left,         "left"},
    {kIrButtonRightCmd, ir_remote::Lane::LOCAL, false, &right,        "right"},
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

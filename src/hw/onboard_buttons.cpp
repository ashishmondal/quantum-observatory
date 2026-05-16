// onboard_buttons — see include/hw/onboard_buttons.h for the contract.

#include "onboard_buttons.h"

#include <Arduino.h>

#include "buzzer.h"
#include "config.h"
#include "mqtt_link.h"
#include "prefs.h"

// Shared cross-core trigger for the FR-17.8 info overlay. Defined in
// src/input/ir_actions.cpp; declared here so we can write it without
// pulling in the full IR action surface.
extern volatile uint32_t g_info_overlay_event_ms;

namespace onboard_buttons {

namespace {

// FR-11.1 -- 30 ms debounce window. The carrier buttons have a
// short physical bounce envelope; 30 ms is comfortably past it
// while still feeling instantaneous to the operator.
constexpr uint32_t kDebounceMs = 30;

// Active-low: pressed = 0, released = 1.
bool     s_menu_pressed_stable = false;
bool     s_menu_pressed_last   = false;
uint32_t s_menu_changed_ms     = 0;

}  // namespace

void begin() {
  // Defensive internal pull-up -- carrier ships external pulls but
  // an unpopulated board would otherwise float. Parallel pulls are
  // benign on the RP2040.
  pinMode(PIN_BTN_MENU, INPUT_PULLUP);
  // Seed the debounce state so the first tick() doesn't fire a
  // phantom press if the button happens to be held during boot.
  const bool pressed_now = digitalRead(PIN_BTN_MENU) == LOW;
  s_menu_pressed_stable = pressed_now;
  s_menu_pressed_last   = pressed_now;
  s_menu_changed_ms     = millis();
  Serial.print("[btn] menu pin=");
  Serial.println(static_cast<int>(PIN_BTN_MENU));
}

void tick(uint32_t now_ms) {
  const bool raw = digitalRead(PIN_BTN_MENU) == LOW;
  if (raw != s_menu_pressed_last) {
    // Edge in the raw input -- restart the debounce window.
    s_menu_pressed_last = raw;
    s_menu_changed_ms   = now_ms;
    return;
  }
  // Raw stable for kDebounceMs? Promote to the debounced state.
  if (raw == s_menu_pressed_stable) return;
  if (static_cast<int32_t>(now_ms - (s_menu_changed_ms + kDebounceMs)) < 0) return;

  s_menu_pressed_stable = raw;

  // Falling edge (released -> pressed) is the accepted event -- same
  // discipline as the IR-press chirp.
  if (!raw) return;  // released

  // FR-10.6 ack chirp, gated by the FR-19 / FR-18.2 v3 button-sound
  // pref so muting button feedback silences the on-board button too.
  if (prefs::current().button_sound) {
    buzzer::chirp();
  }

  // FR-17.8 info overlay trigger -- same primitive the prior IR.4
  // OK binding wrote. Avoid the 0 sentinel which the layer reads
  // as "no event ever fired".
  uint32_t ts = now_ms;
  if (ts == 0u) ts = 1u;
  g_info_overlay_event_ms = ts;

  // FR-11.2 echo -- fire-and-forget. publish_button_event() is a
  // no-op when MQTT is not connected, so the local action above
  // still happens regardless. (Deliberate FR-11.3 divergence -- see
  // header comment.)
  mqtt_link::publish_button_event("menu");

  Serial.println("[btn] menu pressed -> info overlay");
}

}  // namespace onboard_buttons

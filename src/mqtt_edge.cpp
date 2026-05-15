#include "mqtt_edge.h"

#include <Arduino.h>

#include "mqtt_link.h"
#include "scene_state.h"

namespace mqtt_edge {

void tick(uint32_t /*now_ms*/) {
  // FR-5.1: edge-detect on mqtt_link::connected() so we only wake
  // the renderer when the link state actually flips.
  // set_offline_active() itself is already a same-state no-op, but
  // the explicit edge keeps the log single-line per transition.
  static bool s_was_connected  = false;
  static bool s_init_done      = false;
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

}  // namespace mqtt_edge

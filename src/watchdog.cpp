#include "watchdog.h"

#include <Arduino.h>

namespace watchdog {

static constexpr uint32_t kTimeoutMs     = 8000u;
static constexpr uint32_t kRenderStallMs = 4000u;  // half of kTimeoutMs

void begin() {
  rp2040.wdt_begin(kTimeoutMs);
  Serial.print("[wdt] enabled timeout=");
  Serial.print(static_cast<unsigned long>(kTimeoutMs));
  Serial.println("ms");
}

void tick(uint32_t now_ms, uint32_t render_alive_ms) {
  if (render_alive_ms == 0u || (now_ms - render_alive_ms) < kRenderStallMs) {
    rp2040.wdt_reset();
  }
}

}  // namespace watchdog

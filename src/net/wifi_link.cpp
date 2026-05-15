// Implementation of include/wifi_link.h. See header for the FR mapping.
//
// We use the Arduino WiFi class from the earlephilhower core (which wraps
// the LwIP+CYW43 stack) rather than raw cyw43_arch_* calls — the former
// is what every other library in the lib_deps assumes.

#include "wifi_link.h"

#include <Arduino.h>
#include <WiFi.h>

#include "secrets.h"

namespace wifi_link {

namespace {

constexpr uint32_t kBackoffStartMs = 1000u;     // 1 s — FR-5.2
constexpr uint32_t kBackoffMaxMs   = 60000u;    // 60 s — FR-5.2
constexpr uint32_t kConnectTimeoutMs = 15000u;  // give CYW43 15 s per attempt

State    s_state            = State::IDLE;
uint32_t s_attempt_started_ms = 0;
uint32_t s_next_attempt_ms    = 0;
uint32_t s_backoff_ms         = kBackoffStartMs;

void start_attempt(uint32_t now_ms) {
  Serial.print("[wifi] connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  s_state = State::CONNECTING;
  s_attempt_started_ms = now_ms;
}

void on_connected() {
  Serial.print("[wifi] connected ip=");
  Serial.print(WiFi.localIP());
  Serial.print(" rssi=");
  Serial.println(WiFi.RSSI());
  s_state = State::CONNECTED;
  s_backoff_ms = kBackoffStartMs;  // reset on success (FR-5.2)
}

void schedule_retry(uint32_t now_ms, const char* reason) {
  Serial.print("[wifi] ");
  Serial.print(reason);
  Serial.print(" — retry in ");
  Serial.print(s_backoff_ms / 1000u);
  Serial.println("s");
  WiFi.disconnect();
  s_state = State::DISCONNECTED;
  s_next_attempt_ms = now_ms + s_backoff_ms;
  // Double the backoff for next failure, cap at kBackoffMaxMs.
  uint32_t doubled = s_backoff_ms * 2u;
  if (doubled > kBackoffMaxMs) doubled = kBackoffMaxMs;
  s_backoff_ms = doubled;
}

}  // namespace

void begin() {
  if (s_state != State::IDLE) return;  // idempotent
  start_attempt(millis());
}

void poll(uint32_t now_ms) {
  switch (s_state) {
    case State::IDLE:
      // begin() not yet called.
      return;

    case State::CONNECTING: {
      // earlephilhower core returns uint8_t (not wl_status_t) here.
      const uint8_t st = WiFi.status();
      if (st == WL_CONNECTED) {
        on_connected();
      } else if (now_ms - s_attempt_started_ms >= kConnectTimeoutMs) {
        schedule_retry(now_ms, "connect timeout");
      }
      return;
    }

    case State::CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        // Link dropped after being up — restart backoff from the floor
        // since this is a fresh outage, not a chain of failures.
        s_backoff_ms = kBackoffStartMs;
        schedule_retry(now_ms, "link lost");
      }
      return;

    case State::DISCONNECTED:
      if (static_cast<int32_t>(now_ms - s_next_attempt_ms) >= 0) {
        start_attempt(now_ms);
      }
      return;
  }
}

State state() { return s_state; }

bool connected() { return s_state == State::CONNECTED; }

uint32_t backoff_ms() {
  return s_state == State::CONNECTED ? 0u : s_backoff_ms;
}

}  // namespace wifi_link

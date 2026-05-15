// FR-17.8 / IR.4 diagnostic snapshot publisher.
//
// Core 0 publishes IP / RSSI / link state / heap / packed network
// failure mode once per second; Core 1's InfoOverlayLayer reads them
// when rendering the operator overlay. Lives on Core 0 because
// WiFi.* and rp2040.getFreeHeap() are not safe to call from Core 1
// (radio SPI contention with the network stack; both cores hitting
// the malloc subsystem).
//
// All cross-core values are naturally-aligned 32-bit volatiles —
// atomic on RP2040 (CODING_PRACTICES §3). Values may be momentarily
// inconsistent across a publish boundary; for a 5-second-visible
// diagnostic that's fine.

#pragma once

#include <stdint.h>

namespace info_status {

// Recompute and publish all six g_info_* values. Call once per
// second from the Core 0 outer log tick.
void publish();

}  // namespace info_status

// Cross-core snapshot consumed by InfoOverlayLayer (Core 1). Defined
// in info_status_publish.cpp; declared here so consumers don't
// re-declare them piecemeal.
extern volatile uint32_t g_info_ip;          // IPv4 packed: byte[0]<<0 | byte[1]<<8 | ...
extern volatile int32_t  g_info_rssi_dbm;    // 0 when wifi not connected
extern volatile uint32_t g_info_link_flags;  // bit 0 = wifi connected, bit 1 = mqtt connected
extern volatile uint32_t g_info_free_heap_b; // bytes

// Packed network-status byte layout (LE):
//   byte 0: wifi_link::State (IDLE/CONNECTING/CONNECTED/DISCONNECTED)
//   byte 1: mqtt_link::State (IDLE/WAIT_WIFI/CONNECTING/CONNECTED/DISCONNECTED)
//   byte 2: int8_t mqtt rc (PubSubClient::state() at last failure;
//           re-interpret bits as int8_t on the reader side — 0xFE = -2 = SOCKET)
//   byte 3: reserved (0)
extern volatile uint32_t g_info_net_status;
extern volatile uint32_t g_info_net_backoff_s;

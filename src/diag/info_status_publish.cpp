#include "info_status_publish.h"

#include <Arduino.h>
#include <WiFi.h>

#include "mqtt_link.h"
#include "wifi_link.h"

volatile uint32_t g_info_ip          = 0;
volatile int32_t  g_info_rssi_dbm    = 0;
volatile uint32_t g_info_link_flags  = 0;
volatile uint32_t g_info_free_heap_b = 0;
volatile uint32_t g_info_net_status    = 0;
volatile uint32_t g_info_net_backoff_s = 0;

namespace info_status {

void publish() {
  const IPAddress ip = WiFi.localIP();
  g_info_ip = (static_cast<uint32_t>(ip[0])      ) |
              (static_cast<uint32_t>(ip[1]) <<  8) |
              (static_cast<uint32_t>(ip[2]) << 16) |
              (static_cast<uint32_t>(ip[3]) << 24);
  g_info_rssi_dbm    = wifi_link::connected()
                          ? static_cast<int32_t>(WiFi.RSSI())
                          : 0;
  g_info_link_flags  = (wifi_link::connected() ? 0x1u : 0u) |
                       (mqtt_link::connected() ? 0x2u : 0u);
  g_info_free_heap_b = static_cast<uint32_t>(rp2040.getFreeHeap());

  // Packed net status for the IR.4 line-2 error readout. Wi-Fi
  // outage takes precedence — until the link is back up the broker
  // can't be reached anyway, so the MQTT failure mode would be
  // misleading. Once Wi-Fi is up we surface mqtt_link's last_rc()
  // so the panel distinguishes "HA host unreachable" (rc=-2 SOCKET
  // — the boot-loop case that motivated this readout) from "auth
  // refused" (rc=4), "broker overloaded" (rc=3), etc.
  const uint8_t wifi_st = static_cast<uint8_t>(wifi_link::state());
  const uint8_t mqtt_st = static_cast<uint8_t>(mqtt_link::state());
  const uint8_t mqtt_rc = static_cast<uint8_t>(mqtt_link::last_rc());
  g_info_net_status = static_cast<uint32_t>(wifi_st)
                    | (static_cast<uint32_t>(mqtt_st) <<  8)
                    | (static_cast<uint32_t>(mqtt_rc) << 16);

  const uint32_t wifi_backoff_ms = wifi_link::backoff_ms();
  const uint32_t mqtt_backoff_ms = mqtt_link::backoff_ms();
  // Show whichever layer is currently the bottleneck. Wi-Fi down →
  // its backoff is what the operator is waiting on; Wi-Fi up but
  // MQTT retrying → the MQTT backoff is the live timer.
  const uint32_t backoff_ms_eff = wifi_link::connected()
                                    ? mqtt_backoff_ms
                                    : wifi_backoff_ms;
  g_info_net_backoff_s = (backoff_ms_eff + 999u) / 1000u;
}

}  // namespace info_status

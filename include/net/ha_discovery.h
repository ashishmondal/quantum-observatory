// Home Assistant MQTT Discovery publisher (FR-20).
//
// One-shot per session: on every successful MQTT (re)connect,
// publish_all() emits one retained config message per entity to
//
//   <HA_DISCOVERY_PREFIX>/<component>/<MQTT_CLIENT_ID>/<object_id>/config
//
// HA's MQTT integration consumes these to auto-register the device
// and its sensors — zero manual `mqtt: sensor:` YAML required for the
// surfaced entity set. All entities share one `device` block
// (identifiers = MQTT_CLIENT_ID) so HA groups them under a single
// device card, and reference the FR-20.3 availability topic so the
// card greys out within seconds of an outage (broker-side LWT).
//
// State is sourced from the existing 30 s `observatory/status`
// heartbeat via per-entity `value_template` (FR-20.4) — discovery
// does NOT introduce any new state topics or polling cadence.
//
// Pure Core 0; safe to call only from inside mqtt_link's CONNECTING
// success branch (PubSubClient is not thread-safe).

#pragma once

class PubSubClient;

namespace ha_discovery {

// Publish all entity discovery configs against the supplied
// PubSubClient (must already be in the CONNECTED state). Idempotent
// against the broker — configs are retained, so HA recovers them on
// its own restart without us re-publishing.
//
// Each entity is published with retain=true, qos=0. Per-entity
// payloads are sized to fit within the existing PubSubClient buffer
// (mqtt_link::kPubSubBufferSize) — no buffer resize required.
//
// Logs `[ha-disc] pub <topic> <bytes>B` per entity at INFO and a
// single summary line on completion.
void publish_all(PubSubClient& client);

}  // namespace ha_discovery

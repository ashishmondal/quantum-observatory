# Quantum Observatory — Requirements

**Version:** 1.5
**Status:** Draft — pre-implementation
**Target hardware:** Raspberry Pi Pico W + Waveshare RGB-Matrix-P3 (64×32, FM6126A driver, HUB75)
**Stack:** C++ on PlatformIO (earlephilhower Arduino-Pico core), Adafruit Protomatter, MQTT client, Home Assistant integration

---

## 1. Purpose & Scope

A network-connected 64×32 RGB matrix display that renders curated astronomical and ambient scenes. Home Assistant owns all data/logic ("Director"); the Pico W owns rendering ("Cinematographer"). Communication is stateless and intent-based — no raw pixel streaming.

The device also doubles as **the only clock in the room**. The current time MUST be visible at all times — every scene carries a small clock readout, and a dedicated giant-clock scene serves as the default "nothing else to show" view.

**In scope:** scene rendering engine, MQTT scene contract, dual-core orchestration, OTA scene registry, offline fallback, always-on clock readout, dedicated clock/date scene.
**Out of scope:** raw pixel streaming, server-side layout, touchscreen/input handling, audio.

---

## 2. Functional Requirements

### FR-1 Scene Trigger via MQTT
- **FR-1.1** The device SHALL subscribe to a configurable MQTT topic (e.g. `observatory/scene`).
- **FR-1.2** The device SHALL accept JSON payloads conforming to the Scene Contract in §6.
- **FR-1.3** Unknown `scene_id` values SHALL be ignored, logged, and not interrupt the active scene.
- **FR-1.4** Malformed JSON SHALL be rejected without crashing or affecting the current scene.

### FR-2 Scene Lifecycle
- **FR-2.1** Each scene has a `priority` (0–5); higher priorities preempt lower ones.
- **FR-2.2** A scene with `sticky: true` SHALL remain active until (a) a new sticky scene arrives, (b) a `clear_sticky` command is received, or (c) the safety TTL expires.
- **FR-2.3** Non-sticky scenes SHALL auto-expire after `duration` seconds (default 30) and revert to the default scene.
- **FR-2.4** All scenes SHALL be capped by a hard TTL (default 1 hour) to prevent permanent lock.

### FR-3 Rendering
- **FR-3.1** The device SHALL maintain a target frame rate of **20–30 FPS** during normal operation.
- **FR-3.2** The renderer SHALL composite three layers: Ambient (background), Information (text), Transition (effects).
- **FR-3.3** Text SHALL be drawn with a destructive halo / bounding box to remain legible over animated backgrounds.
- **FR-3.4** The device SHALL support at minimum the following ambient backgrounds: 3-level parallax starfield, Perlin-noise nebula.
- **FR-3.5** The device SHALL support at minimum the following transitions: instant cut, dissolve, warp.

### FR-4 Typography
- **FR-4.1** The device SHALL render text using `Silkscreen` (5×7) for data and `Space Mono Bold` (or equivalent compact bold) for headers.
- **FR-4.2** Character set SHALL be ASCII 0–127, stored in flash (PROGMEM).
- **FR-4.3** Display SHALL render at most **2 lines** of text simultaneously.
- **FR-4.4** Text strings exceeding ~14 characters per line SHALL be truncated; truncation logic is the **Director's** responsibility, not the Pico's.

### FR-5 Offline Fallback
- **FR-5.1** On MQTT disconnect, the device SHALL render an "Offline Observation" scene (local clock + starfield) within 5 seconds.
- **FR-5.2** The device SHALL automatically reconnect to MQTT with exponential backoff (1s → 60s max).
- **FR-5.3** The device SHALL never display a blank screen during normal operation (always at least the offline scene).

### FR-6 OTA & Scene Registry
- **FR-6.1** Scenes SHALL be defined as parameter sets in a Scene Registry table (e.g. `{bg_type, text_effect, palette_id}`).
- **FR-6.2** OTA updates SHALL be supported (ArduinoOTA or similar over Wi-Fi).
- **FR-6.3** The most common OTA payload SHALL be a refreshed Scene Registry, not full firmware.

### FR-7 Ambient & Thermal Safety Modes
- **FR-7.1** The firmware SHALL sample the on-board photoresistor (ADC0 / GP26) at ≥ 1 Hz as the ambient-light source.
- **FR-7.2** When the photoresistor reading drops below the configured night threshold (with hysteresis), the firmware SHALL switch to the dedicated `night` scene (low-light room-clock readout). When the reading rises back above `threshold + hysteresis`, the firmware SHALL revert to the previously active scene (or the default `clock` scene if none).
- **FR-7.3** The firmware SHALL sample the on-board DS3231 on-die temperature register (0x11) at ≥ 0.1 Hz. When the reading exceeds the configured thermal threshold, the firmware SHALL switch to the dedicated `thermal_safe` scene (very-low-light + textual indication of overheat) until the reading drops below `threshold − hysteresis`.
- **FR-7.4** Night and thermal thresholds SHALL be remotely tunable via MQTT — topic `observatory/night`, payload `{"threshold": N, "hysteresis": M}` (raw 12-bit ADC units for night; degrees Celsius for thermal via `observatory/thermal` with the same shape). HA owns policy; firmware owns the comparison and scene swap.
- **FR-7.5** Mode priority (highest preempts lower): `thermal_safe` > `night` > MQTT-requested scene > default. Mode swaps SHALL bypass the FR-2 priority field — they are firmware-owned safety/ambient overrides, not Director intent.
- **FR-7.6** On boot, before any successful sensor read, the firmware SHALL behave as if both modes are inactive (i.e. show the MQTT-requested or default scene). Defaults SHALL be conservative (night threshold so daylight does not trigger; thermal threshold well above ambient room temperature) and live in `config.h`.

### FR-8 Display Initialization
- **FR-8.1** The firmware SHALL run the FM6126A C12/C13 register init sequence before invoking Protomatter `begin()`. *(Confirmed required for the Waveshare panel.)*

### FR-9 Always-On Clock (room-clock duty)
- **FR-9.1** The current time SHALL be visible on the panel at all times during normal operation, regardless of which scene is active. (This device is the room's only clock.)
- **FR-9.2** Every scene SHALL include a small clock readout (HH:MM, 24-hour by default) rendered as part of the standard scene chrome — typically in a corner, ≤ ~15 px wide, with halo for legibility (FR-3.3).
- **FR-9.3** The clock readout is rendered by a shared chrome helper, not duplicated in each scene's `render()`. Scenes opt out only with explicit justification (e.g., a transition mid-frame); opting out SHALL last no longer than ~2 seconds.
- **FR-9.4** A dedicated `clock` scene SHALL exist that fills the panel with a giant time readout (HH:MM) plus the date (e.g. `WED 01 MAY`) on a second line. This is the default scene shown when nothing else is active and is the visual fallback paired with `offline` (FR-5.1).
- **FR-9.5** Time SHALL be sourced from the on-board DS3231 RTC (I²C1, addr 0x68) at all times — at startup, during steady-state, and after any reboot. The RTC is the single read path for the wall clock; nothing else (MQTT, NTP, `millis()`-since-boot) is ever consulted as a time source. The local `millis()` counter is used only for sub-second smoothing between RTC reads.
  - Home Assistant pushes corrections over MQTT (`observatory/time`, payload = epoch seconds UTC + tz offset minutes, published at least once per hour). On receipt the firmware SHALL write the value into the RTC; subsequent reads then naturally pick it up. MQTT is the *correction* path, never the *read* path.
  - Rationale: the RTC is battery-backed and survives Wi-Fi/MQTT outages and reboots, so the room-clock duty (FR-9.1) keeps working when HA is down. Treating it as the single read source also removes a class of bugs where boot-time scenes display nothing because MQTT hasn't connected yet.
- **FR-9.6** Until the RTC has been read at least once after boot (or its oscillator-stop flag is set, indicating loss of backup power), the clock readout SHALL render `--:--` rather than a wrong time. Scenes still render normally; only the readout is masked.

### FR-10 Audible Alerts (on-board buzzer)
- **FR-10.1** The firmware SHALL drive the on-board buzzer (GP27, active-high) for short attention chirps tied to high-priority scenes (priority ≥ 4, e.g. `weather_alert`, `iss_pass`).
- **FR-10.2** Buzzer behaviour (mute / chirp pattern) SHALL be remotely controllable via MQTT (e.g. `observatory/buzzer`, payload = `{"mode": "off"|"chirp"|"siren", "count": N}`). HA owns the policy; firmware owns the timing.
- **FR-10.3** A firmware mute switch SHALL hard-cap any chirp to ≤ 200 ms ON / ≥ 800 ms OFF and ≤ 3 chirps per scene activation, regardless of MQTT command, to avoid runaway noise from a malformed payload.
- **FR-10.4** The buzzer SHALL default to off after boot. A boot-time self-test chirp is allowed but MUST be ≤ 50 ms.

### FR-11 Local Input (on-board buttons)
- **FR-11.1** The firmware SHALL read the three on-board buttons (GP15 = MENU, GP19 = DOWN, GP21 = UP) with debounce ≥ 30 ms.
- **FR-11.2** When MQTT is connected, button presses SHALL be published to HA (e.g. `observatory/button`, payload = `"menu"|"down"|"up"`) so HA can drive scene response. Local behaviour SHALL be a no-op in this mode (Director still owns intent).
- **FR-11.3** When MQTT is disconnected, the buttons SHALL provide a minimal local fallback: MENU cycles between `clock` and `offline` scenes; UP/DOWN reserved for future local actions (no firmware-managed brightness in v1).

---

## 3. Non-Functional Requirements

### NFR-1 Performance
- **NFR-1.1** Rendering SHALL not stutter visibly when MQTT messages arrive at up to 5 msg/sec.
- **NFR-1.2** End-to-end latency from MQTT publish to first frame of new scene SHALL be ≤ 250 ms.
- **NFR-1.3** Trigonometric and noise functions SHALL use fixed-point math and/or precomputed LUTs; software-emulated `float` is prohibited in render loops.

### NFR-2 Memory
- **NFR-2.1** RP2040 has 264 KB SRAM; firmware SHALL leave ≥ 32 KB free at runtime.
- **NFR-2.2** Dynamic allocation (`new`, `malloc`, `String` concatenation) SHALL NOT occur in the main render or MQTT loops after `setup()`.
- **NFR-2.3** JSON parsing SHALL use a fixed-size `StaticJsonDocument` sized for the largest documented Scene Contract payload + 25% headroom.

### NFR-3 Reliability
- **NFR-3.1** The device SHALL recover from Wi-Fi loss without reboot.
- **NFR-3.2** Watchdog timer SHALL reset the device if either core stalls > 8 seconds.
- **NFR-3.3** No buffer overruns, regardless of malformed network input (fuzz-tested).

### NFR-4 Thermal
- **NFR-4.1** The firmware SHALL keep the panel under safe operating temperature in a closed enclosure via the FR-7.3 thermal-safe mode (DS3231 on-die temperature → dim scene swap above threshold). Scenes are expected to use moderate sustained colour values; full-white sustained fills are out of bounds.

### NFR-5 Maintainability
- **NFR-5.1** Adding a new scene SHALL require only: (a) one entry in the Scene Registry, (b) one `render_*()` function. No changes to MQTT, dispatch, or core split logic.
- **NFR-5.2** Pin assignments and panel geometry SHALL be centralized in a single `config.h`.

---

## 4. Architecture

### 4.1 Dual-Core Split

| Core | Role | Responsibilities |
|---|---|---|
| **Core 0 — Gatekeeper** | Network & state | Wi-Fi mgmt, MQTT pub/sub, JSON parsing, Scene Registry lookup, writes to shared `SceneState` struct, watchdog feed |
| **Core 1 — Artist** | Rendering only | Reads `SceneState`, runs render pipeline, drives Protomatter, maintains FPS |

### 4.2 Inter-Core Communication
- A single `SceneState` struct in shared SRAM, guarded by a `mutex_t` (Pico SDK).
- Core 0 writes; Core 1 reads. Updates are coarse-grained (entire struct copy) to minimize lock contention.
- A "scene_dirty" flag triggers Core 1 to re-initialize per-scene state at next frame boundary.

### 4.3 Render Pipeline (Core 1, per frame)
1. Acquire current `SceneState` snapshot.
2. Clear back buffer.
3. Draw Ambient layer (per scene `bg_type`).
4. Draw Information layer (text with halo).
5. Apply Transition layer if mid-transition.
6. Call `matrix.show()` — Protomatter handles bit-plane refresh in background.

---

## 5. Data Contracts

### 5.1 Scene Trigger (HA → Pico)

Topic: `observatory/scene`
Payload (JSON):

```json
{
  "scene_id": "jupiter_visibility",
  "priority": 3,
  "duration": 30,
  "sticky": false,
  "overrides": {
    "text": "Visible: East @ 9PM",
    "val": "78"
  }
}
```

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `scene_id` | string | yes | — | Must exist in Scene Registry |
| `priority` | int 0–5 | no | 1 | Higher preempts lower |
| `duration` | int seconds | no | 30 | Ignored if `sticky: true` |
| `sticky` | bool | no | false | Persists until cleared or TTL |
| `overrides` | object | no | `{}` | Scene-specific params (`text`, `val`, `color`, …) |

### 5.2 Night & Thermal Mode (HA ↔ Pico)
Topic: `observatory/night` — payload `{"threshold": N, "hysteresis": M}` (12-bit ADC units, both 0–4095).
Topic: `observatory/thermal` — payload `{"threshold": N, "hysteresis": M}` (degrees Celsius, integer).
Firmware persists the most recent values in RAM only; defaults from `config.h` apply on boot until HA pushes an update.

### 5.3 Sticky Clear (HA → Pico)
Topic: `observatory/clear_sticky` — payload: empty.

### 5.4 Status (Pico → HA)
Topic: `observatory/status` — JSON heartbeat every 30 s:
```json
{ "scene_id": "...", "fps": 28, "rssi": -55, "uptime_s": 1234, "free_heap": 180000 }
```

---

## 6. Initial Scene Registry (target set for v1.0)

| scene_id | bg_type | text_layout | notes |
|---|---|---|---|
| `boot` | starfield | "OBS" centered | shown at startup |
| `clock` | starfield_dim | giant HH:MM + date line | default / idle scene (FR-9.4) |
| `offline` | starfield_dim | local time | MQTT disconnect fallback |
| `night` | black | dim HH:MM only | LDR-triggered (FR-7.2); preempts MQTT scenes |
| `thermal_safe` | black | dim "COOL DOWN" + temperature | DS3231-triggered (FR-7.3); preempts everything |
| `iss_pass` | nebula | 2-line: "ISS NOW" + direction | priority 4 |
| `moon_phase` | starfield | phase glyph + name | sticky |
| `jupiter_visibility` | nebula | direction + time | example in §5.1 |
| `weather_alert` | red_pulse | 2-line warning | priority 5 (max), triggers buzzer chirp (FR-10.1) |

All scenes above (except possibly `boot` during the splash window) carry the standard small clock readout per FR-9.2.

---

## 7. Phased Roadmap

| Phase | Deliverable | Exit criteria |
|---|---|---|
| **P1 — Dual-core split** | Core 0/1 IPC, dummy MQTT load | Starfield holds ≥ 25 FPS while Core 0 receives 5 msg/s |
| **P2 — Graphics library** | Custom fonts, halo text, scene base class | "Hello" legible over moving stars |
| **P2.5 — Clock substrate** | Time source, chrome clock readout, giant `clock` scene | Panel shows correct HH:MM in every scene; idle = giant clock |
| **P3 — HA integration** | Real ISS/moon/planet sensors, MQTT topics | All registry scenes triggerable from HA |
| **P4 — Polish** | Warp/dissolve transitions, auto-dim, OTA registry refresh | OTA scene addition without firmware reflash |

---

## 8. Risks & Mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| RAM exhaustion (264 KB tight with double-buffer + JSON) | High | Single buffer where possible; static allocation only; budget tracked per scene |
| Thermal damage from long runs in a closed enclosure | Medium | DS3231 on-die temp → firmware swaps to `thermal_safe` low-light scene above threshold (FR-7.3); scene authors avoid sustained full-white fills |
| Wi-Fi instability stalling render | Medium | Dual-core split (NFR-3.1); watchdog (NFR-3.2) |
| FM6126A init lost after brownout | Low | Re-run init on every `setup()`; consider periodic re-init if blank detected |
| Float math creeping into render loop | Medium | Code review checklist; CI grep for `float`/`sin(`/`cos(` in `render_*` files |
| OTA bricking device | Medium | Dual-partition OTA with rollback; physical BOOTSEL recovery documented |

---

## 9. Open Questions

1. **OTA mechanism**: ArduinoOTA, HA-served HTTP, or MQTT-payload chunked? (Decision needed before P4.)
2. **Scene Registry storage**: compiled-in vs. LittleFS-loaded JSON? (Affects OTA strategy.)
3. ~~**Time source**: NTP directly from Pico, or pushed from HA?~~ **Resolved (v1.4):** on-board DS3231 RTC is primary; MQTT pushes corrections (FR-9.5).
4. **Wi-Fi credentials provisioning**: hardcoded, WiFiManager portal, or HA-pushed? (Security implication.)
5. **Authentication for MQTT**: username/password vs. TLS client cert? (HA broker capability dependent.)
6. **What happens if two equal-priority scenes arrive in quick succession**: latest wins, or queue?
7. **Night-mode threshold defaults** (FR-7.2 / FR-7.6): the vendor demo uses `adc_read() - 700` as a dark-floor offset; we should re-baseline raw photoresistor values in our actual enclosure and pick a sensible default + hysteresis before shipping FR-7.
8. **Thermal threshold default** (FR-7.3): DS3231 on-die temperature is internal silicon, not panel surface — needs a one-time correlation against an IR thermometer reading on the panel itself to pick a meaningful threshold (the DS3231 will read cooler than the LEDs).
9. **Buzzer pattern vocabulary** (FR-10.2): just `off`/`chirp`/`siren`, or a richer pattern grammar? Start minimal; extend if HA needs it.

---

## 10. Glossary

- **Director**: Home Assistant — owns data, scheduling, and intent.
- **Cinematographer**: Pico W firmware — owns rendering and timing.
- **Scene**: a self-contained visual program identified by `scene_id`.
- **Scene Contract**: the JSON schema in §5.1.
- **Sticky**: a scene that does not auto-expire on `duration`.
- **TTL**: hard maximum lifetime for any scene (default 1 hour).
- **Destructive Overlay**: drawing technique where text writes opaque halo pixels into the background layer to guarantee legibility.

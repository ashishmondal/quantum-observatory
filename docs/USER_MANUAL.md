# Quantum Observatory — User Manual

A guide to what the device does and how to use it, covering only the features that are implemented and shipping today. Cross-references point at deeper engineering docs ([REQUIREMENTS.md](REQUIREMENTS.md), [PLAN.md](PLAN.md), [MQTT_TOPICS.md](MQTT_TOPICS.md), [THEME.md](THEME.md), [HARDWARE.md](HARDWARE.md)).

---

## 1. What it is

A 64×32 RGB LED matrix wall display that pairs astronomical "what's overhead right now" scenes with always-on room-clock duty. **Home Assistant** is the data + scheduling brain; the **Pico W** is the renderer. You can use it standalone (clock + offline scene), but the observatory features assume HA + an MQTT broker on the same network.

The display is always doing something. It boots into a branded splash, hands off to a giant LCD-style clock the moment HA is reachable, and either rotates through scenes on a schedule or shows whatever you (or HA) ask it to.

---

## 2. First-time setup

### 2.1 Power on

1. Plug the panel into power. After ~1 s you see the **Quantum Observatory** splash artwork.
2. The splash stays up until the device connects to your Wi-Fi and your MQTT broker.
3. As soon as the broker connects, the splash gives way to the giant clock.

The device is silent throughout boot — no beeps or chirps until the splash clears (see [§7 Sound](#7-sound)).

### 2.2 Wi-Fi + broker

Wi-Fi SSID, password, MQTT broker host/port, and credentials are baked into the firmware at build time via `include/secrets.h` (copy from `include/secrets.h.example` and rebuild). To change them you reflash.

### 2.3 Home Assistant first pairing

Two pieces of HA setup, both one-time:

1. **Run the setup helper** (from the repo root, with HA URL + a long-lived token):
   ```sh
   python homeassistant/setup_mqtt.py --all \
     --url http://homeassistant.local:8123 \
     --token "$HA_TOKEN"
   ```
   This installs `homeassistant/packages/quantum_observatory.yaml` (theme + image-tint controls, ISS REST sensor, evening rotation, time-correction automation) and the `homeassistant/pyscript/observatory_publisher.py` ephemeris publisher (Jupiter / Moon / constellation / ISS-pass prediction via `skyfield`).
2. **Wait ~10 seconds after the first MQTT connect.** The firmware self-registers as a Home Assistant device via MQTT-Discovery — no manual sensor configuration. You'll see a new **Quantum Observatory** device card under Settings → Devices with 14 entities ([§8 Home Assistant integration](#8-home-assistant-integration)).

### 2.4 Time

The device reads time from an on-board DS3231 real-time clock backed by a coin cell, so the wall-clock duty keeps working through Wi-Fi outages and power cycles. HA pushes a correction once an hour on `observatory/time` (set up automatically by the package above); the firmware writes it to the RTC and re-reads. Until the RTC has been read at least once, the clock readout shows `--:--`.

---

## 3. The display at a glance

Each scene draws into a layered compositor. Most of the time you only see the scene itself, but four overlay slots can engage on top of it:

| Layer | When you see it | Purpose |
|---|---|---|
| **Scene** | always | the active scene's main content |
| **Chrome** (corner clock) | every scene except the giant clock | small `HH:MM` readout so the room-clock duty is preserved |
| **Safety overlay** | when night / thermal-hot / offline / boot-splash is engaged | dimmed or replacement view; underlying scene keeps animating beneath |
| **Info overlay** | 5 s after pressing **OK** (remote) or the on-board **MENU** button | IP, RSSI, MQTT state, uptime, FPS, scene, theme, free heap |
| **Settings overlay** | while the menu is open | full-panel menu over a Bayer-dimmed backdrop |
| **Transition** | 250 ms during scene swaps | fade-through-black between scenes |

Pressing the same button again closes overlays (info, settings).

---

## 4. Scenes

Scenes are picked by Home Assistant, by the IR remote ▲/▼, or by the firmware itself (safety overrides). A scene with no override and no Director request defaults to `clock`.

### 4.1 Scenes you'll see day-to-day

| Wire id | What it shows | Where it comes from |
|---|---|---|
| `clock` | Giant LCD-style HH:MM with "18 88" ghost segments, animated minute-roll, 1 Hz colon pulse, themed background motion, date below | default; what you'll see most of the time |
| `moon_phase` | Phase glyph + bucket name (NEW / WAX CRES / FIRST QTR / WAX GIB / FULL / WAN GIB / LAST QTR / WAN CRES), illumination % | HA pyscript publishes real synodic phase every 6 h |
| `jupiter_visibility` | Bearing + elevation (`VIS 090x45`), magnitude, distance; `BELOW` when below the horizon; `IN <IAU>` (host constellation) in daylight | HA pyscript every 15 min using `skyfield` + DE421 |
| `constellation_now` | Real (RA, Dec) star projection of the constellation overhead at your latitude, with brightest stars sized + named; pulsing red `+` cross marks a highlighted star | HA pyscript hourly; falls back to local 30 s rotation if HA is silent |
| `iss_pass` | When the ISS is visible: `VIS <bearing>x<elevation>` look-angle + `CREW N`. Otherwise a countdown: `VIS IN 3D` / `VIS IN 5H` / `VIS IN 12M` / `VIS SOON` | HA pyscript every 30 s; visibility is the three-way AND of (ISS sunlit) AND (sun ≤ −6° at observer) AND (ISS above horizon) |

The evening rotation automation walks `clock → moon_phase → jupiter_visibility → iss_pass → constellation_now` every 5 minutes from sunset until 22:30 local, then steps back so the LDR-driven night mode can take over.

### 4.2 Safety / status scenes (firmware-driven)

These engage automatically. You don't normally pick them.

| Wire id | When it engages | What it shows |
|---|---|---|
| `night` | LDR reading goes dark for ~1 s | very dim HH:MM on black, no chrome |
| `thermal_safe` | DS3231 die temp exceeds 50 °C | "COOL DOWN" + current temperature, very dim |
| `offline` | MQTT broker unreachable for ~5 s | clock + dim starfield + status hint |
| `splash` | boot, until first MQTT connect | the observatory artwork |

Override priority is **splash > thermal > night > offline > director**. Engagement does **not** restart the underlying scene — it keeps animating behind the override and resumes the instant the override clears.

### 4.3 Diagnostic scenes (publish to switch in)

```
mosquitto_pub -t observatory/scene -m '{"scene_id":"gfx_test"}'
```

| Wire id | Use |
|---|---|
| `gfx_test` | Smooth gradient + palette ramps + FPS / SHIFT counters + a 1-pixel red "jitter witness" |
| `font_demo` | Cycles every loaded font; ◄/► picks fonts in this scene only |
| `ir_test` | Live IR decode counters + last decode + 5 s rolling health bar (used during initial remote learning) |

---

## 5. Themes

Six retro sci-fi looks. Each bundles inks, fonts, header decorations, and a signature animated background for the giant clock; switches happen in one frame with no scene re-init.

| Wire id | Vibe | Header font | Background |
|---|---|---|---|
| `apollo_amber` | NASA mission control on a Tek scope | Press Start 2P | starfield |
| `nostromo_green` | green phosphor CRT (Alien) | NokiaFC22 | drifting nebula |
| `vectrex_neon` | neon-glow vector arcade | Pixel Operator | perspective grid |
| `blade_runner` | cyan/orange + frame border | Pixel Operator | rain + glow |
| `lcars_tos` | block bars in place of brackets, Trek-era LCARS | Pixel Operator | LCARS sweep |
| `section_nine` | Ghost in the Shell, blue/orange technical | Pixel Operator | grid |

**Pick a theme:** rotate with ◄/► on the remote, or use the **Observatory theme** dropdown that appears in Home Assistant. The choice persists across power cycles (`/prefs.json` on the device's LittleFS partition, with a wear-protected writeback — see [§9 Persistence](#9-persistence)).

**Image tint** is a 0..100 % slider that controls how strongly non-Apollo themes recolour photographic backgrounds. Default 50 leaves the original photo recognisable with a soft theme cast; 0 = the original baked palette under any theme; 100 = full duotone retoning. Adjust from the on-panel settings menu (▲/▼ on `DISPLAY → BG TINT`) or from the HA **Observatory image tint** slider. Apollo is unaffected at every value.

---

## 6. Controls

### 6.1 IR remote (8-button Roku-style)

Out of the box the firmware expects the remote characterised during build (NEC address `0xC2EA`). If you swap in a different remote, run the IR-learning wizard:

```sh
mosquitto_pub -t observatory/scene -m '{"scene_id":"ir_test"}'
```

…walk through every prompt on the panel; the device publishes a one-shot JSON document on `observatory/debug` with the captured command codes. Paste them into `include/config.h` and rebuild.

| Button | When menu is **closed** | When menu is **open** |
|---|---|---|
| **▲ / ▼** | cycle through the operator scene list: `CLOCK → MOON_PHASE → JUPITER_VISIBILITY → CONSTELLATION_NOW → ISS_PASS` | move row up / down |
| **◄ / ►** | cycle themes (wraps); in `font_demo` picks fonts instead | adjust value (toggle, cycle, or tint step) |
| **OK** | (no global action) | commit / enter category |
| **Back** | clear any sticky scene and return to `CLOCK` | leave category → ROOT → close menu |
| **Home** | jump immediately to `CLOCK`, force-closes the menu | close menu, then jump to `CLOCK` |
| **\* (Options)** | open the settings menu | close the settings menu |

A short audible chirp confirms each accepted press (when the button-sound preference is on and the device isn't in night mode). Held buttons produce one chirp then silent repeats — the cycle only advances on the first frame of the press, not on every IR repeat frame.

### 6.2 On-board MENU button

GP15 on the carrier, active-low. Pressing it locally shows the **info overlay** for 5 s — same content as a remote OK press would have shown before the OK button got rebound for the settings menu. The press is also echoed as `{"button":"menu"}` on `observatory/button` so HA can log it. The local action fires regardless of MQTT state — useful when the broker is down and you're trying to diagnose why.

### 6.3 Info overlay

Five seconds of "what's going on right now" over whatever scene is active:

```
IP    192.168.1.42
RSSI  -55 dBm  MQ OK
UP    4h12m   FPS 24
SCN   clock   THM apo
HEAP  180.3k
```

When the network isn't fully online, line 2 swaps to a precise short tag (`WIFI JOIN`, `WIFI DOWN 8s`, `MQ WAIT`, `MQ SOCKET 4s`, `MQ AUTH`, `MQ DENY`, `MQ TMOUT`, `MQ LOST`, etc.) so you can diagnose from across the room. A trailing `*` after the theme id (`THM apo*`) means a preference change hasn't been durably saved yet — expect it to clear within ~35 s of the change.

Press OK or MENU again to dismiss early.

### 6.4 Settings menu

Press **\*** (Options) on the remote to open. Two categories:

**DISPLAY**
- **BG TINT** — 11-step segmented bar (0..100 % in 10 % increments). ◄/► steps; rail clamps at the ends play a low "thunk". A small live-preview swatch shows the underlying image with the candidate tint so you can judge before committing.

**SOUND**
- **THEME** — on/off, gates the FR-10.7 theme melody fired when the look-and-feel changes.
- **BUTTON** — on/off, gates every IR / on-board button chirp **and** every settings-menu cue (so you can mute the menu itself for night fiddling).
- **TICK** — `NONE / MIN / 10MIN / HOUR`, gates the giant-clock digit-roll click cascade.

Navigation:

| Button | Action |
|---|---|
| ▲ / ▼ | move row up/down (or pick category) |
| ◄ / ► | adjust value |
| OK | commit (toggle on boolean rows, cycle on `TICK`, enter category from ROOT) |
| Back | leave category → ROOT → close |
| \* | close |
| Home | close + jump to CLOCK |

The menu paints a fully opaque black backdrop over the whole panel (except the BG TINT preview swatch). The corner clock chrome is suppressed while the menu is up — the menu owns the full 64×32. If you walk away, the menu auto-closes after 30 s with a 5 s countdown shown in the bottom-right.

Every value change goes through the persistent-prefs layer. About 5 s after your last edit you'll see a `SAVED` toast bottom-right confirming the change made it to flash.

---

## 7. Sound

The device drives a small piezo buzzer on GP27. All audible cues sit above 8 kHz so they read as "device ticks" rather than competing with music or speech.

### 7.1 What makes noise

| Cue | When | Gate |
|---|---|---|
| Short chirp | every accepted IR remote button press, on-board MENU press | `button_sound = on` |
| Theme melody (~1–2 s) | whenever the active theme actually changes (idempotent re-sets are silent) | `theme_sound = on` |
| Tick click | each minute-roll digit step on the giant clock | `tick_sound_mode != NONE` (selects MIN / 10MIN / HOUR) |
| Settings cues | open/close jingles, knob-pitch clicks during value edits, rail thunks | `button_sound = on` |

### 7.2 What's silent

Two global silencers compose with logical OR — audio plays only when **both** are inactive:

- **Boot quiet** — silence from power-on until the splash overlay clears (first successful MQTT connect). The Westminster boot melody from early prototypes is gone; the first wiring witness is whatever cue you fire after the splash clears in a lit room.
- **Night quiet** — silence while the LDR reports darkness. Cover the LDR mid-melody and the buzzer cuts immediately; uncover and cues resume on the next event.

Cues fired while quiet are **dropped at the driver boundary**, not queued — so the device never plays a stale "I beeped while you were asleep" cue when night quiet lifts.

There is no global MQTT mute today; if you want one, use the SOUND menu to disable each class individually.

---

## 8. Home Assistant integration

### 8.1 The device card

After the first connect, HA shows a single **Quantum Observatory** device with these auto-discovered entities (all read from the 30 s heartbeat — no extra MQTT traffic per entity):

| Entity | Class | What it tells you |
|---|---|---|
| `sensor.*_scene` | — | active scene wire id |
| `sensor.*_temperature` | temperature (°C) | DS3231 die temp; `unknown` until first read |
| `sensor.*_light_raw` | diagnostic | 12-bit ADC reading from the photoresistor (HIGHER = darker on this board) |
| `binary_sensor.*_night` | light | LDR-driven night-mode flag |
| `binary_sensor.*_thermal_hot` | heat (diagnostic) | thermal-safe override active |
| `binary_sensor.*_prefs_dirty` | problem (diagnostic) | a setting hasn't been durably saved yet |
| `sensor.*_fps`, `_render_slack`, `_uptime`, `_free_heap`, `_rssi` | diagnostic | render + system health |
| `sensor.*_mqtt_state`, `_mqtt_rc`, `_mqtt_backoff` | diagnostic | link-layer state |

Availability is driven by an MQTT Last-Will-Testament on `observatory/availability`: the firmware publishes retained `online` on connect, the broker publishes retained `offline` automatically when the device falls off (within ~15 s). The whole device card greys out together.

If you keep long-running HA history, exclude the diagnostic sensors from the recorder — they change every heartbeat. The exact `recorder: exclude:` block is documented in [MQTT_TOPICS.md](MQTT_TOPICS.md) under "HA recorder advisory".

### 8.2 Manually-installed entities (not discovered)

The HA package installs two write-back controls that stay manually configured:

- **`select.observatory_theme`** — dropdown for the six themes.
- **`number.observatory_image_tint`** — 0..100 slider.

Both publish to `observatory/theme`; the firmware echoes the active values back via `observatory/status` so the HA UI always reflects ground truth.

### 8.3 Useful operator commands

```sh
# pick a scene
mosquitto_pub -t observatory/scene \
  -m '{"scene_id":"jupiter_visibility","priority":2,"sticky":true}'

# release a sticky scene
mosquitto_pub -t observatory/clear_sticky -m ''

# pick a theme
mosquitto_pub -t observatory/theme -m '{"id":"section_nine"}'

# tint preference only
mosquitto_pub -t observatory/theme -m '{"tint":30}'

# tune the night-mode threshold (raw 12-bit ADC)
mosquitto_pub -t observatory/night -m '{"threshold":3800,"hysteresis":200}'

# tune the thermal threshold (°C)
mosquitto_pub -t observatory/thermal -m '{"threshold":50,"hysteresis":5}'

# factory-reset persisted prefs (deletes /prefs.json, reboots)
mosquitto_pub -t observatory/prefs/reset -n
```

Full topic / payload contract: [MQTT_TOPICS.md](MQTT_TOPICS.md).

---

## 9. Persistence

The device keeps a small `/prefs.json` on its LittleFS partition. v3 schema:

```json
{ "v": 3, "theme": "section_nine", "image_tint_pct": 50,
  "theme_sound": true, "button_sound": true, "tick_sound_mode": 1 }
```

Writes are **wear-protected**: every setting change marks the cache dirty, waits 5 s for further edits to settle, then writes at most once every 30 s. Spamming ◄/► through every theme for 10 seconds produces exactly one flash write. The `prefs_dirty` binary_sensor in HA — and the trailing `*` on the info overlay — show the brief window before the write lands; expect it to clear within 35 s of the last change.

A v1 or v2 file from an older firmware loads cleanly under v3 with defaults for the missing keys (forward-compat passthrough).

To reset to factory defaults (`apollo_amber`, tint 50, all sounds on, tick MIN): publish to `observatory/prefs/reset` — the device deletes the file and reboots.

---

## 10. Day-to-day quirks worth knowing

- **The clock never displays a known-wrong time.** Until the RTC has been read at least once after boot — or if its oscillator-stop flag fires (loss of backup power) — the readout is masked to `--:--`. Other scenes still render.
- **Sticky scenes survive forever (almost).** A scene published with `sticky: true` stays up until you clear it, a higher-priority scene preempts it, or the 1-hour safety TTL fires.
- **The night override doesn't restart your scene.** Covering the LDR mid-`constellation_now` dims the panel; uncovering resumes the same constellation reveal where it left off.
- **HA outages don't break the clock.** RTC keeps ticking through Wi-Fi drops and power cycles. The `offline` scene takes over within ~5 s of an MQTT disconnect; recovery is automatic with 1 s → 60 s exponential backoff.
- **The buzzer is silent during boot and during night mode**, no exceptions. If you want to confirm the buzzer is wired correctly, press a remote button after the splash clears in a lit room.
- **Theme melodies fire only on *change*.** Publishing the same theme id twice is silent.

---

## 11. Troubleshooting

| Symptom | What to check |
|---|---|
| Stuck on splash artwork | Wi-Fi or MQTT broker not reachable. The boot splash latches until the first successful MQTT connect. Check the info overlay (on-board MENU button) — line 2 will tell you exactly which step is stuck (`WIFI JOIN` / `MQ SOCKET 4s` etc.). |
| Panel shows `offline` scene | MQTT broker unreachable for ≥ 5 s. Info overlay's line 2 names the failure mode; backoff seconds count down. |
| Clock shows `--:--` | DS3231 not yet read, or oscillator-stop flag set (loss of backup power). Check the coin cell. Once a fresh time is written to `observatory/time` (or HA pushes the hourly correction), it resolves. |
| ISS scene shows `WAIT` | No fresh ISS payload in the last 1 h. HA pyscript publisher down, or `sensor.iss_position` stuck `unavailable`. |
| Jupiter shows `BELOW` in daylight you expect to see it | Working as intended — visibility AND requires sun ≤ −6° at the observer. The line will read `IN <IAU>` (host constellation) in daylight when Jupiter is above the horizon. |
| Stuck in `night` mode | Light sensor reading is in the dark range. Cover/uncover briefly to test, or tune the threshold via `observatory/night`. |
| IR remote does nothing | Bright scene + cheap IR receiver = noise. The `ir_test` diagnostic scene shows live decode + parity counters; if `parity` / `unknown` dominate, you've got EMI. Also confirm the remote's NEC address matches `IR_REMOTE_ADDR_EXPECTED` in `config.h`. |
| Settings menu opens but ▲/▼ cycles scenes instead | Likely a stale firmware build — the IR carve-out is conditional on `settings_ui::is_open()`. Reflash. |
| Theme switch is silent | Either the theme matched the active one (idempotent → silent) or `theme_sound` is off in the SOUND menu / device is in night mode / device is still in the boot-quiet window. |
| HA device card greyed out | Broker emitted the LWT `offline` — the device dropped the keepalive. Check Wi-Fi + power. |
| Theme / tint change doesn't survive a power cycle | Check the info overlay for `THM apo*` (trailing `*` = unflushed). Wait 35 s after the last change. If it persists, LittleFS write may have failed — try `observatory/prefs/reset`. |

---

## 12. Where to read more

- **[REQUIREMENTS.md](REQUIREMENTS.md)** — what the system does, FR by FR.
- **[PLAN.md](PLAN.md)** — phased implementation roadmap with live status.
- **[MQTT_TOPICS.md](MQTT_TOPICS.md)** — wire contracts for every topic the device subscribes / publishes, including HA Discovery details.
- **[THEME.md](THEME.md)** — full theming spec, asset authoring rules, per-theme ink/font tables.
- **[HARDWARE.md](HARDWARE.md)** — pin map, peripherals, wiring notes, IR remote table.
- **[CODING_PRACTICES.md](CODING_PRACTICES.md)** — rules every code change follows (memory, numeric, concurrency discipline).
- **[FUTURE_SCENES.md](FUTURE_SCENES.md)** — the long-tail scene backlog.

# Quantum Observatory — Implementation Plan

Companion to [REQUIREMENTS.md](REQUIREMENTS.md). Each step is a **small, demoable win** — finish it, see it work, check the box, move on.

**Legend:** `[ ]` not started · `[~]` in progress · `[x]` done · `[!]` blocked

---

## Phase 0 — Foundation (already done ✅)

- [x] **0.1** PlatformIO project on earlephilhower core, Adafruit Protomatter installed
- [x] **0.2** Pin map matched to existing wiring (R1=2, G1=3, B1=4, R2=5, G2=8, B2=9, A=10, B=16, C=18, D=20, CLK=11, LAT=12, OE=13)
- [x] **0.3** FM6126A C12/C13 init sequence working — panel lights up
- [x] **0.4** Serial logging from `loop()` (works regardless of when monitor connects)

---

## Phase 1 — Render Plumbing (single core, no network)

> Goal: prove the graphics pipeline before adding any complexity.

- [x] **1.1 Frame timer & FPS counter**
  - Add an FPS counter that prints to serial every second.
  - **Win:** see "FPS: 60" (or whatever) in the log.

- [x] **1.2 Config split**
  - Extract pin map and panel geometry to `include/config.h`.
  - **Win:** `main.cpp` no longer holds magic pin numbers.

- [x] **1.3 Scene base class & dispatcher**
  - Create `Scene` interface with `init()`, `render(uint32_t ms)`, `name()`.
  - One global "current scene" pointer; `loop()` calls `render()`.
  - **Win:** can swap scenes by changing one line.

- [x] **1.4 First real scene: solid color clock**
  - Scene draws `millis() / 1000` as digits using built-in GFX font.
  - **Win:** seconds counter visible on panel.

- [x] **1.5 Boot scene: "OBS" centered**
  - Static centered text on starless background.
  - **Win:** branded splash.

---

## Phase 2 — Ambient Backgrounds

> Goal: pretty things to look at.

- [x] **2.1 Fixed-point math helpers**
  - `int16_t` Q8.8 add/mul, sin/cos LUT (256 entries).
  - **Win:** unit-tested on host or via serial print.

- [x] **2.2 Single-layer starfield**
  - 30 stars, random positions, twinkle by varying brightness.
  - **Win:** twinkling sky.

- [x] **2.3 3-level parallax starfield**
  - 3 star layers moving at different horizontal speeds.
  - **Win:** depth illusion.

- [x] **2.4 Perlin nebula**
  - 16×8 noise grid, bilinear-upscaled to 64×32, slow time animation.
  - **Win:** breathing gas clouds.

- [x] **2.5 Background selector**
  - `enum BgType { NONE, STARFIELD, PARALLAX, NEBULA }` + dispatch.
  - **Win:** scenes can pick their background.

---

## Phase 3 — Typography & Legibility

- [x] **3.1 Embed Silkscreen 5×7 font**
  - Convert TTF → GFXfont header (PROGMEM), ASCII 32–126.
  - **Win:** Silkscreen visible on panel.

- [x] **3.2 Embed Space Mono Bold (header font)**
  - Same conversion, larger size for headers.
  - **Win:** two distinct typefaces selectable.

- [x] **3.3 Destructive halo text helper**
  - `drawTextHalo(x, y, str, fg, halo)` — draws halo offsets first, then fg on top.
  - **Win:** text is legible over moving starfield.

- [x] **3.4 Two-line layout primitive**
  - Header on row 0–7, data on row 9–16. Horizontal centering helper.
  - **Win:** clean 2-line example scene.

---

## Phase 3.5 — Clock Substrate (render side)

> Goal: device starts earning its keep as the room clock. Render side only — wall-clock arrives over MQTT in 5.6.

- [x] **3.5.1 TimeOfDay clock state**
  - Shared `struct TimeOfDay { int32_t epoch_utc; int16_t tz_offset_min; uint32_t set_at_ms; bool valid; }` plus `now_hhmm(uint32_t now_ms, uint8_t* h, uint8_t* m)` helper that interpolates via `millis()` delta. Starts invalid → callers render `--:--`. (FR-9.5, FR-9.6)
  - **Win:** serial dump shows HH:MM advancing once a fake epoch is poked in.

- [x] **3.5.2 Chrome clock readout helper**
  - `gfx::draw_clock_chrome(matrix)` — Picopixel HH:MM in a fixed corner with halo. Reads the shared `TimeOfDay`. Wired into the per-frame render path so every scene picks it up without per-scene edits. (FR-9.2, FR-9.3, FR-3.3)
  - **Win:** every existing demo scene gains a tiny clock in the corner.

- [x] **3.5.3 Giant `clock` scene**
  - Header: HH:MM in the largest font that fits ~rows 0–18. Body: `WED 01 MAY` in Picopixel. Background: dim starfield. (FR-9.4)
  - **Win:** dedicated room-clock scene, legible across the room.

---

## Phase 3.6 — RTC Integration (FR-9.5 conformance)

> Drift fix: 3.5.1 currently stores wall-clock in RAM and projects via
> `millis()`, which violates FR-9.5 ("RTC is the single read path; nothing
> else — MQTT, NTP, `millis()`-since-boot — is ever consulted as a time
> source"). This phase retrofits the on-board DS3231 (I²C1, GP6/GP7,
> addr 0x68) as the real source. Render code keeps the same
> `tod::now()` API — only the implementation changes.

- [x] **3.6.1 DS3231 driver**
  - `include/ds3231.h` + `src/ds3231.cpp`. I²C1 init on GP6/GP7 @ 400 kHz; read 7 BCD registers in one transaction; decode to `epoch_utc`; expose `oscillator_stopped()` (status reg 0x0F bit 7) and a `write(epoch_utc)` for the MQTT correction path. No dynamic alloc; pure integer math.
  - **Win:** serial dump shows RTC HH:MM:SS once a second, independent of any MQTT/fake-epoch poke.

- [x] **3.6.2 Rewire `tod` to read from RTC**
  - `tod::now()` returns a snapshot built from `last_rtc_epoch + (millis() - last_rtc_read_ms) / 1000` (sub-second smoothing only, per FR-9.5). A 1 Hz background poll (Core 0) refreshes `last_rtc_epoch`. `valid` tracks (a) at-least-one-successful-read AND (b) `!oscillator_stopped()` (FR-9.6). Remove `tod::set_from_mqtt()` from the read path entirely; it becomes a thin wrapper that writes the RTC then forces a re-read.
  - Remove the fake-epoch poke in `setup()` ([src/main.cpp](../src/main.cpp)) — the RTC already holds a battery-backed value.
  - **Win:** chrome + giant clock display the RTC time across a power cycle (coin cell keeps it alive); MQTT is not involved.

- [x] **3.6.3 Local timezone**
  - DS3231 stores naive seconds (we choose to store *local* time directly so reads need no tz state). Decide: store local-time epoch in RTC vs. store UTC + tz in flash/LittleFS. Document the choice in HARDWARE.md and apply consistently in `ds3231::write()` and `tod::now()`.
  - **Win:** RTC value matches wall-clock to the minute after a manual `mosquitto_pub` to `observatory/time`.

---

## Phase 4 — Dual-Core Split

> Goal: network noise can't stutter the render.

- [x] **4.1 Move render to Core 1**
  - `setup1()` / `loop1()` runs the matrix; `loop()` (Core 0) just sleeps.
  - **Win:** FPS counter still shows ~30, proving Core 1 is rendering.

- [x] **4.2 Shared `SceneState` struct + mutex**
  - Define struct, mutex_t, getter/setter helpers.
  - **Win:** Core 0 toggles `scene_id` every 5 s, Core 1 swaps scenes.

- [x] **4.3 Stress test**
  - Hammer Core 0 with a tight loop doing fake JSON parsing.
  - **Win:** Core 1 FPS unchanged → architecture validated.

---

## Phase 5 — Wi-Fi & MQTT (Network MVP)

- [x] **5.1 Wi-Fi connect**
  - Hardcoded SSID/pass in `secrets.h` (gitignored).
  - **Win:** serial prints IP address.

- [x] **5.2 MQTT connect & status heartbeat**
  - Publish `observatory/status` JSON every 30 s.
  - **Win:** message visible in MQTT Explorer / HA.

- [x] **5.3 Subscribe to `observatory/scene`**
  - Parse JSON with `StaticJsonDocument`, log `scene_id`.
  - **Win:** publish a test message from HA, see it logged.

- [x] **5.4 Wire MQTT → SceneState**
  - Incoming `scene_id` updates the shared struct; Core 1 picks it up.
  - **Win:** publishing `{"scene_id":"boot"}` switches the panel.

- [ ] **5.5 Night & thermal-safe modes (FR-7 end to end)**

  Split into three sub-steps mirroring the natural work boundaries — LDR
  hardware → temperature hardware → MQTT tuning. Each lands a visible
  win on its own; the order matters because 5.5.3's tunables only make
  sense once 5.5.1/5.5.2 actually consume them.

  - [x] **5.5.1 LDR + night scene + dispatcher priority** (FR-7.1, FR-7.2, FR-7.5, FR-7.6)
    - `light_sensor` module: ADC0 / GP26 init on Core 0, sampled at ≥ 1 Hz with hysteresis around `LIGHT_NIGHT_THRESHOLD` / `LIGHT_NIGHT_HYSTERESIS` defaults from `config.h`.
    - New `night` scene (very dim HH:MM on black, no chrome).
    - Mode-state in shared `scene_state` (or sibling `mode_state` namespace) holding `night_active` + `mqtt_requested` (the scene the Director last asked for, so we can revert). Render-side dispatcher resolves: `night_active ? NIGHT : mqtt_requested ?? CLOCK` each frame. Bypasses FR-2 priority (firmware-owned safety override).
    - **Win:** covering the LDR with a finger swaps the panel to dim `night` within ~1 s; uncovering reverts to whatever scene HA last asked for.

  - [x] **5.5.2 DS3231 temp + thermal_safe scene** (FR-7.3, FR-7.5, NFR-4.1)
    - Extend `ds3231` with `read_temp_c()` (reg 0x11, integer °C; vendor demo throws away the 0x12 fractional byte).
    - Background poll at ≥ 0.1 Hz on Core 0 with hysteresis around `THERMAL_THRESHOLD_C` / `THERMAL_HYSTERESIS_C` defaults.
    - New `thermal_safe` scene (very dim "COOL DOWN" + current temp, on black).
    - Dispatcher priority: `thermal_active > night_active > mqtt_requested > default` (FR-7.5). `thermal_active` preempts even `night`.
    - Diagnostic: log `[thermal] c=NN` every 30 s while polling so we have data toward Open Question §9.8 (DS3231-vs-panel correlation).
    - **Win:** warming the DS3231 chip with a finger for ~30 s eventually swaps to `thermal_safe`; lifting the finger reverts; serial log shows the temperature climbing/falling.

  - [x] **5.5.3 MQTT threshold topics** (FR-7.4)
    - Subscribe to `observatory/night` and `observatory/thermal`, both `{"threshold":N,"hysteresis":M}`. Same FR-1.4 validation pattern as `observatory/scene`. Apply atomically to the live thresholds.
    - **Win:** publishing `observatory/night '{"threshold":2000,"hysteresis":150}'` makes the panel trip into night mode in normal room light; restoring saner values reverts.

- [ ] **5.6 Subscribe to `observatory/time` (RTC correction path)**
  - Parse epoch UTC + tz offset minutes; call `ds3231::write(...)` to persist into the RTC; the next `tod` background poll picks it up naturally. MQTT is the *correction* path, never the *read* path (FR-9.5).
  - Depends on Phase 3.6 — without the RTC driver, nothing to write to.
  - **Win:** publishing a stale time to `observatory/time`, then power-cycling the device, comes back up displaying the corrected time from the RTC with no MQTT needed.

---

## Phase 6 — Scene Lifecycle & Resilience

- [ ] **6.1 Priority preemption**
  - Reject incoming scenes with lower priority than current.
  - **Win:** spam of priority-1 scenes can't override an active priority-5.

- [ ] **6.2 Duration & TTL expiry**
  - Non-sticky scenes auto-revert to default after `duration` seconds; all scenes hard-cap at 1 h.
  - Default scene SHALL be `clock` (FR-9.4), not `boot`. `boot` is one-shot at startup only.
  - **Win:** test scene fades back to the giant clock on schedule.

- [ ] **6.3 Sticky + clear_sticky**
  - Sticky scenes survive duration; `observatory/clear_sticky` resets them.
  - **Win:** moon phase scene stays until cleared.

- [ ] **6.4 Offline fallback**
  - On MQTT disconnect, switch to "offline" scene (clock + dim starfield).
  - Auto-reconnect with exponential backoff.
  - **Win:** unplug router → panel shows offline scene → replug → recovers.

- [ ] **6.5 Watchdog**
  - 8-second WDT on both cores.
  - **Win:** deliberately stalling Core 1 reboots the device.

---

## Phase 7 — First Real "Observatory" Scenes

> One scene = one win. Pick whichever motivates you most each session.

- [ ] **7.1** `iss_pass` — "ISS NOW" header + direction
- [ ] **7.2** `moon_phase` — phase glyph + name (sticky)
- [ ] **7.3** `jupiter_visibility` — direction + time
- [ ] **7.4** `weather_alert` — red-pulse background, 2-line warning (priority 5)
- [ ] **7.5** Home Assistant automations & sensors that publish them

---

## Phase 8 — Polish

- [ ] **8.1 Dissolve transition** between scenes
- [ ] **8.2 Warp transition** (pixel stretch)
- [ ] **8.3 OTA firmware update** (ArduinoOTA)
- [ ] **8.4 Scene Registry as data, not code** — load from LittleFS so adding scenes doesn't need a flash

---

## Phase 9 — Hardening (final)

- [ ] **9.1 Memory audit** — log free heap; confirm ≥ 32 KB headroom under all scenes
- [ ] **9.2 Fuzz MQTT input** — random bytes, oversized payloads, malformed JSON
- [ ] **9.3 24 h soak test** — leave running with cycling scenes; check FPS, heap, reconnects
- [ ] **9.4 Thermal check** — IR thermometer on panel after 1 h running the brightest production scene; correlate against DS3231 reading and tune the FR-7.3 threshold
- [ ] **9.5 Tag v1.0 release**

---

## Open Decisions to Resolve Before They Block Us

Tracked from REQUIREMENTS.md §9 — answer when the relevant phase begins:

- [x] Phase 5: MQTT auth (user/pass vs TLS)
- [x] Phase 5: Wi-Fi credential provisioning (hardcoded vs portal)
- [ ] Phase 6: equal-priority collision behavior (latest-wins vs queue)
- [x] Phase 7: ~~time source (NTP vs HA push)~~ — resolved v1.3: HA pushes via MQTT (`observatory/time`)
- [ ] Phase 8: OTA mechanism (ArduinoOTA vs HA HTTP vs MQTT chunks)
- [ ] Phase 8: Scene Registry storage (compiled vs LittleFS)
- [x] Phase 5.5: night-mode + thermal threshold defaults (need in-enclosure calibration vs IR-thermometer correlation)

---

## Working Rhythm

- One phase ≠ one session. Most steps above should fit in **30–90 minutes**.
- Always end a session with **something visibly different** on the panel or in the log.
- After each `[x]`, commit with message: `phase X.Y: <what works now>`.

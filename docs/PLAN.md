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

## Phase 3.7 — Color & Background System (FR-12)

> Goal: smooth gradients on a 5-bit panel, palette cycling for free animation,
> and a drop-in artwork pipeline so non-coders can author backgrounds.
> Landed out of order — driven by visible flicker + banding while exercising
> Phase 5 scenes.

- [x] **3.7.1 Flicker fix — Serial off Core 1 + bit depth tune**
  - Symptom: visible per-row flicker even on static scenes. Root causes: (1) `Serial.print` on Core 1 was preempting Protomatter's PIO/DMA refresh; (2) `PANEL_BIT_DEPTH=6` left refresh rate too low (~110 Hz) for the eye on this panel.
  - Fix: all logging moved to Core 0; Core 1 publishes `volatile uint32_t g_render_fps` for Core 0 to print. `PANEL_BIT_DEPTH` lowered 6→5 (refresh ~190 Hz, one fewer plane).
  - **Win:** rock-steady frame on every scene; FPS line still printed every second.

- [x] **3.7.2 Split-layout palette LUT system** (FR-12.1, FR-12.2)
  - `include/color_palette.h` + `src/color_palette.cpp`. 256-entry palette tables built once at boot from compact stop lists; `BG_LEN=192` cyclic + `FG_LEN=64` linear. APIs: `palette::bg(Id, idx, shift)`, `palette::fg(Id, br6)`, `palette::init_all()`. Round-to-nearest RGB565 packing; **no gamma encoding** — stops are perceptual sRGB-ish (the powf(x,2.2) collapsed the dim end on this panel).
  - Ids: STAR_WHITE / STAR_BLUE / STAR_AMBER / NIGHT_SKY (FG); NEBULA_CLOUDS (BG).
  - **Win:** smooth full-width brightness ramps with no banding from index 0 upward.

- [x] **3.7.3 Static deep-sky starfield + twinkle overlay**
  - Replaced animated starfield with a static field (seed 0xC0FFEE, 90 dust / 28 small / 10 medium / 4 hero) over a dim navy floor (`palette::fg(NIGHT_SKY, 28)`); 6 twinkles cycle in/out (trapezoidal envelope, 1.8–5.5 s lifetimes, brightness cap FG 48). Tuned to user reference image.
  - **Win:** looks like the night sky, not a screensaver.

- [x] **3.7.4 Palette-cycled nebula** (FR-12.3)
  - NebulaBg now indexes into `palette::bg(NEBULA_CLOUDS, idx, m_palette_shift)`; shift advances `dt>>4`. Pixels themselves are unchanged frame-to-frame in the cyclic baseline; the palette walks.
  - **Win:** clouds drift with zero per-pixel work beyond the bilinear sample.

- [x] **3.7.5 BitmapBg (procedural palette-indexed)** (FR-12.4)
  - `src/backgrounds/bitmap_bg.h`. 2 KB owned RAM (1 byte/pixel), `Region {start, length, speed}` table (max 4, signed steps/sec). `init_generated(fn, pal, regions, count)` for procedural fills; demo cycles two bands in opposite directions.
  - **Win:** proves multi-region cycling on one image at independent speeds.

- [x] **3.7.6 ImagePaletteBg (flash-resident, zero RAM copy)** (FR-12.4)
  - `src/backgrounds/image_palette_bg.h`. Three flash pointers — per-image 192-entry palette, 2 KB pixel array, region table. `set(palette, pixels, regions, region_count)`. Same cyclic shift as BitmapBg.
  - **Win:** images cost ~2.4 KB flash each, 0 bytes of RAM beyond the pointers.

- [x] **3.7.7 BMP → header asset pipeline** (FR-12.5, FR-12.7, NFR-5.3)
  - `tools/bmp_to_header.py` (stdlib only): validates 8-bit indexed uncompressed 64×32 BMP, **hard-rejects any pixel index ≥ 192**, optional `assets/<name>.regions` sidecar (`start length speed` per line; default static `{0, max(idx)+1, 0}`).
  - Emits `include/bitmaps/<name>.h` with `k<Name>Palette[192]` / `k<Name>Pixels[64*32]` / `k<Name>Regions[]` and always-rebuilt `include/bitmaps/_index.h` with `kImageRegistry[]` of `ImageEntry`.
  - `tools/pre_build.py` PlatformIO hook (`extra_scripts = pre:tools/pre_build.py`) runs the converter on every build.
  - `assets/README.md` documents the GIMP/Photoshop indexed-BMP authoring path.
  - **Win:** drop `assets/foo.bmp` into the repo, hit Build, `bg_image` shows it. No source edits.

- [x] **3.7.8 Scene wiring — BG_BITMAP, BG_IMAGE, GFX_TEST** (FR-12.6)
  - SceneId additions; `id_from_string` map updated (`bg_bitmap`, `bg_image`, `gfx_test`). `Backgrounds::init_image_default()` points the image bg at `kImageRegistry[0]` if any.
  - `src/scenes/gfx_test_scene.h`: cycling nebula gradient + 3 FG ramps (white/blue/amber) + Picopixel FPS/SHIFT/uptime + 1-pixel red "jitter witness" hopping each frame; `wants_clock_chrome=false`; self-contained 1 s window FPS sampler.
  - **Win:** `mosquitto_pub ... '{"scene_id":"gfx_test"}'` brings up the diagnostic; visible regressions show up immediately.

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

- [x] **5.5 Night & thermal-safe modes (FR-7 end to end)**

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

- [x] **5.6 Subscribe to `observatory/time` (RTC correction path)**
  - Parse epoch UTC + tz offset minutes; call `ds3231::write(...)` to persist into the RTC; the next `tod` background poll picks it up naturally. MQTT is the *correction* path, never the *read* path (FR-9.5).
  - Depends on Phase 3.6 — without the RTC driver, nothing to write to.
  - **Win:** publishing a stale time to `observatory/time`, then power-cycling the device, comes back up displaying the corrected time from the RTC with no MQTT needed.

---

## Phase 6 — Scene Lifecycle & Resilience

- [x] **6.1 Priority preemption**
  - Reject incoming scenes with lower priority than current.
  - **Win:** spam of priority-1 scenes can't override an active priority-5.

- [x] **6.2 Duration & TTL expiry**
  - Non-sticky scenes auto-revert to default after `duration` seconds; all scenes hard-cap at 1 h.
  - Default scene SHALL be `clock` (FR-9.4), not `boot`. `boot` is one-shot at startup only.
  - **Win:** test scene fades back to the giant clock on schedule.

- [x] **6.3 Sticky + clear_sticky**
  - Sticky scenes survive duration; `observatory/clear_sticky` resets them.
  - **Win:** moon phase scene stays until cleared.

- [x] **6.4 Offline fallback**
  - On MQTT disconnect, switch to "offline" scene (clock + dim starfield).
  - Auto-reconnect with exponential backoff.
  - **Win:** unplug router → panel shows offline scene → replug → recovers.

- [x] **6.5 Watchdog**
  - 8-second WDT on both cores.
  - **Win:** deliberately stalling Core 1 reboots the device.

---

## Phase 6.5 — Polish: Splash, RTC Hardening, Sky Background (FR-13)

> Visual + reliability polish wave landed between the network MVP and
> the first real "observatory" scenes. Driven by user-visible glitches
> (sporadic wrong-time flashes, washed-out splash artwork) and a desire
> to make the idle clock scene feel alive without distracting from the
> time readout.

- [x] **6.5.1 Digital-7 LCD typography for the giant clock** (FR-9.4)
  - Converted Digital-7 TTF → GFXfont header (`include/fonts/digital_7__mono_14pt7b.h`), `#pragma once` + `static const` linkage so it can be included from multiple TUs without multiple-definition errors. Giant clock + offline scenes share the font; ghost layer ("18:88") drawn first WITH halo, live HH:MM drawn on top WITHOUT halo. Dim-green divider row + deep-amber Picopixel date strip.
  - **Win:** clock scene reads as a real LCD wall clock across the room.

- [x] **6.5.2 DS3231 reliability stack** (FR-13.5)
  - Layered defenses: (a) `gpio_pull_up()` from `<hardware/gpio.h>` on SDA/SCL — `pinMode(INPUT_PULLUP)` was breaking the I²C alt-function and causing every read to fail (panel kept tripping into OFFLINE); (b) per-burst sanity-clamp on BCD fields; (c) two-burst consensus read in `ds3231::read()` accepting only if Δseconds ∈ [0,1]; (d) `tod::poll_validated()` rejecting any value diverging from the projected time by > 3 hours; (e) outer poll cadence = 1 hour on accept, 1s→2s→…→1h backoff on reject.
  - **Win:** sporadic single-frame "1:03 SUN 22 JUN" flashes eliminated; serial log goes hours between successful polls with no rejections.

- [x] **6.5.3 Boot splash** (FR-13.1)
  - `assets/observatory.bmp` rendered via a dedicated `SplashScene` that owns a local `ImagePaletteBg`. New `SceneId::SPLASH` is firmware-only (NOT in `kIdMap`) and `set_splash_active()` resolves first in the dispatcher — preempts thermal/night/offline/MQTT. Latched clear on first MQTT connect; subsequent disconnects do not re-show.
  - `Backgrounds::init_image_default()` rewritten to look up the image background by name ("starfield") instead of `kImageRegistry[0]`, so adding observatory.bmp doesn't break the clock background fallback.
  - **Win:** branded splash on boot, clean handoff to giant clock the moment HA is reachable.

- [x] **6.5.4 Gamma-correct BMP pipeline** (FR-12.8)
  - `tools/bmp_to_header.py`: `GAMMA = 2.2` constant + `gamma_correct()` applied to each R/G/B channel before quantising to RGB565.
  - **Win:** observatory splash and any future artwork render with perceptually correct brightness on the panel's non-linear LED response.

- [x] **6.5.5 Sky background — gradient + sun** (FR-13.2, FR-13.3)
  - NOAA low-precision solar model (`include/sun_position.h` / `src/sun_position.cpp`, ~80 lines, ±1°). Five-band altitude gradient (day / golden / civil / nautical / astronomical), per-row top→bottom lerp, smoothed top rows so deep-night doesn't show a hard near-black band at row 0.
  - Sun: 4-tier disc (radius 4, ~9 px), each tier carrying top/bot color pair → vertical gradient pale-on-top / warm-on-bottom, intensifying near the horizon. Linear azimuth→x mapping (az 60°→0, 180°→32, 300°→63) draws a true semi-elliptical arc; sx clamped to `[4, 59]` so the disc is never cropped.
  - Houston lat/lon + tz hardcoded in `config.h` (LOCAL_TZ_OFFSET_MIN flips for DST manually until MQTT-settable lands).
  - Refactored into a free function `sky_bg_render::draw(matrix, utc_epoch, lat, lon)` so the timelapse can feed a synthetic epoch without mocking `tod::now()`.
  - **Win:** giant clock now sits over a live sun-aware sky that visibly evolves through the day.

- [x] **6.5.6 Sky timelapse debug scene** (FR-13.4)
  - `SkyTimelapseScene` maps `now_ms % 10000` to a 24 h synthetic UTC sweep and feeds `sky_bg_render::draw()`. Cyan "TIMELAPSE" Picopixel label at y=31. Selectable via standard MQTT scene contract (`{"scene_id":"sky_timelapse"}`).
  - **Win:** one full day (sun rising on the left, arcing up over centre, setting on the right, full night, repeat) every 10 s — makes tuning the gradient and sun arc trivial.

---

## Phase 7 — First Real "Observatory" Scenes

> One scene = one win. Pick whichever motivates you most each session.

- [x] **7.1** `iss_pass` — typewriter ALT/CREW/VIS, on-device look-angle + visibility derivation (FR-14)
- [x] **7.2** `moon_phase` — phase glyph + name (sticky)
- [x] **7.3** `jupiter_visibility` — direction + time
- [x] **7.4** `constellation_now` — overhead constellation art + name (sticky); HA picks current overhead constellation by date + observer lat/lon. See [FUTURE_SCENES.md](FUTURE_SCENES.md) for the long-tail scene backlog.
- [ ] **7.5** Home Assistant automations & sensors that publish them

---

## Phase 8 — Polish

- [ ] **8.1 Dissolve transition** between scenes
- [ ] **8.2 Warp transition** (pixel stretch)
- [ ] **8.3 OTA firmware update** (ArduinoOTA)
- [ ] **8.4 Scene Registry as data, not code** — load from LittleFS so adding scenes doesn't need a flash

---

## Phase T — Retro Sci-Fi Theming System (FR-15)

Full design in [THEME.md](THEME.md). Five themes (`apollo_amber` default,
`nostromo_green`, `vectrex_neon`, `blade_runner`, `lcars_tos`); each
bundles inks + fonts + brackets + layout hints + a duotone BG ramp.
Scenes consume `theme::*`, never hardcode color/font/brackets.

- [x] **T.1 Docs** — THEME.md drafted; FR-15 added to REQUIREMENTS; README link; assets/README.md authoring note (FR-15.6 runtime duotone).
- [x] **T.2 `theme.h` / `theme.cpp` skeleton** — APOLLO_AMBER only, exact same colors / fonts / brackets as today. `theme::set/current/ink/font/has/bracket_open/bracket_close/bg_palette_for`. Atomic `uint8_t` active id. Built and called from one no-op site (e.g. `gfx_test`) to prove the API. Build fixes during D.1: `GFXfont` is a typedef'd anonymous struct in Adafruit_GFX, can't be forward-declared, so theme.h pulls `<gfxfont.h>` directly; theme.cpp `#undef`s Arduino's `bit(b)` macro before defining `theme::bit(Hint)`. **Exit:** firmware builds, runs, looks pixel-identical to today.
- [ ] **T.3 Scene refactor** — replace every hardcoded RGB565 / `setFont(&...)` / bracket literal under `src/scenes/` with `theme::ink()` / `theme::font()` / `theme::bracket_*()`. Mechanical, every scene file touched. **Exit:** `grep -nE '0x[0-9A-Fa-f]{4}|setFont\\(' src/scenes/` returns nothing meaningful; visual diff = zero.
- [ ] **T.4 MQTT theme topic** — subscribe `observatory/theme` `{"id":"<theme_id>"}` in `mqtt_link.cpp`; persist active theme in `scene_state` (no flash); add `theme` to `observatory/status` heartbeat; add HA `select.observatory_theme` in `homeassistant/setup_mqtt.py`. **Exit:** publishing the topic with `apollo_amber` is a no-op; unknown ids logged + ignored.
- [ ] **T.5 NOSTROMO_GREEN** — second theme: green CRT inks, scanlines hint, cursor-block hint. Reuses existing fonts (no new TTFs yet). First *visible* theme switch from MQTT. **Exit:** publishing `nostromo_green` flips the dashboard end-to-end inside one frame.
- [ ] **T.6 Font roster** — convert and bundle Silkscreen, VT323, Pixel Operator, Pixel Operator Bold, Press Start 2P, Vector Battle to GFXfont headers under `include/fonts/`. Update Apollo header to Press Start 2P (closes FR-4.1 placeholder). License attribution stubs in each header. **Exit:** all six headers compile; PROGMEM cost ≤ 20 KB total.
- [ ] **T.7 Remaining themes** — VECTREX_NEON (vector-glow halo), BLADE_RUNNER (cyan/orange + frame border), LCARS_TOS (block bars, no brackets). Each adds at least one new layout hint primitive in `gfx_text.h`. **Exit:** all five themes selectable; each visually distinct at a glance.
- [ ] **T.8 BG duotone runtime** — `tools/bmp_to_header.py` emits per-image `lum[192]` + reads `assets/<name>.notheme` sidecar → `themeable` flag. Boot-time 256-entry ramp LUT per non-default theme. Per-image double-buffered runtime palette (~3.8 KB SRAM). Theme switch ≤ 5 ms. APOLLO stays passthrough. **Exit:** switching to NOSTROMO retones every themable BMP green; switching back restores original colors; no torn frames.
- [ ] **T.9 `gfx_test` coverage** — extend the diagnostic scene to cycle every `theme::Ink` role and every `Hint` overlay on a fixed cadence so one capture covers all themes (FR-15.8). **Exit:** running `gfx_test` for 60 s exercises every theme at least once.

---

## Phase D — Dual-Core Compositor & Idle-Slack Utilization (FR-16)

> Goal: graduate Core 1 from "single-scene renderer with idle slack" to
> "compositor + ambient sky simulator + speculative pre-render", and
> tighten the cross-core data path so Core 0's network jitter cannot
> perturb the frame. Each step is a small demoable win with a visible
> or measurable outcome — no flag-day rewrites. Order matters: D.1
> must land before D.2/D.3 (it builds the layer plumbing they consume),
> and D.4 must land before D.6 (seqlock is needed before sky-model
> snapshots cross cores at high frequency).

- [x] **D.1 Layer stack scaffolding** (FR-16.1, FR-16.10)
  - Introduced a `Layer` interface (`render(matrix, now_ms)`, `name()`,
    optional `prepare()`) in [src/scenes/layer.h](../src/scenes/layer.h).
    Refactored `loop1()` to walk a fixed `Layer*` array indexed by
    `LayerSlot` enum `[FG, OVERLAY_SAFETY, OVERLAY_TRANSITION, CHROME]`
    instead of calling `g_current_scene->render()` directly. Two
    file-scope adapters in [src/main.cpp](../src/main.cpp): `SceneFgLayer`
    delegates to whatever `g_current_scene` points at (so adding a
    scene stays "registry entry + render function" — NFR-5.1
    preserved); `ChromeLayer` honours `wants_clock_chrome()` for the
    giant-clock opt-out. Overlay slots start `nullptr` for D.2/D.3/D.8
    to fill without touching `loop1()`.
  - **Win:** zero visual change; firmware builds clean (RAM 30.1%,
    Flash 25.5%); one layer of indirection now exists so D.2/D.3/D.6
    are local edits.

- [ ] **D.2 Crossfade transitions** (FR-16.3)
  - On `take_pending()`, instead of swapping `g_current_scene` instantly,
    install a `CrossfadeLayer` in `LAYER_OVERLAY_TRANSITION` that owns
    the outgoing scene + incoming scene + a 250 ms integer alpha ramp.
    Both scenes render to scratch; the layer blends per pixel using a
    Q8.8 alpha. After the ramp, `g_current_scene` becomes the incoming
    scene and the crossfade layer self-removes. Hard-cut remains
    available as a per-scene opt-out.
  - **Win:** publishing two `observatory/scene` messages back-to-back
    produces a visible 250 ms fade between scenes instead of a hard cut.

- [ ] **D.3 Safety overrides as overlays** (FR-16.2)
  - Convert `NIGHT`, `OFFLINE`, `THERMAL_SAFE`, `SPLASH` from
    dispatcher-preempting `SceneId`s into compositor overlay layers
    (in `LAYER_OVERLAY_SAFETY`) with their own fade-in/fade-out
    envelope (~200 ms). The dispatcher's "active scene" is unaffected
    by override engagement; it remains whatever the Director or default
    policy chose. `scene_state` setters (`set_night_active`,
    `set_thermal_active`, `set_offline_active`, `set_splash_active`)
    flip overlay-layer visibility instead of forcing scene swaps.
    FR-7.5 priority preserved (THERMAL > SPLASH > OFFLINE > NIGHT >
    none).
  - **Win:** cover the LDR mid-`ConstellationNow` reveal, then
    uncover — the constellation animation continues from where it was,
    no restart. Same for thermal/offline/splash transitions.

- [ ] **D.4 Seqlock cross-core snapshots** (FR-16.7)
  - Introduce `seq_snapshot<T>` helper (single producer Core 0, single
    consumer Core 1, retry on torn read). Migrate the `scene_state`
    read path Core 1 uses each frame off `mutex_t` and onto seqlock.
    Keep the mutex for write-write coordination on Core 0 (MQTT
    callback vs. `scene_state::tick`). Generalizes the pattern already
    proven for `g_render_alive_ms` / `g_render_fps`.
  - **Win:** `[render] fps=` line stays flat under the existing
    `CORE0_STRESS` build flag *and* a new `CORE0_MQTT_FLOOD` test that
    hammers `observatory/scene` at 20 msg/s. No mutex contention in
    Core 1's hot path.

- [ ] **D.5 Idle-slack instrumentation** (FR-16.9)
  - Measure per-frame `kFrameIntervalMs − render_time` on Core 1.
    Maintain a 32-frame rolling average; publish to Core 0 via a
    `volatile uint32_t g_render_slack_ms`. Add `render_slack_ms` to
    the `observatory/status` heartbeat. Define `kSlackFloorMs`
    (default 8) below which D.6/D.7 work skips for the frame.
  - **Win:** HA shows a live `render_slack_ms` sensor; idle scenes
    report ~30 ms slack, heavy scenes report < 10 ms — quantifies how
    much budget D.6/D.7 actually have.

- [ ] **D.6 Continuous sky-model on Core 1** (FR-16.5)
  - Run the `sun_position` computation + a moon-phase calculation +
    (if available) the cached ISS look-angle once per second on Core 1
    during a slack window, regardless of active scene. Publish into a
    `sky_snapshot` struct via the FR-16.7 seqlock. Sky-aware scenes
    (`clock`+sky bg, `moon_phase`, `iss_pass`, `jupiter_visibility`)
    read the snapshot instead of recomputing; the chrome layer gains
    a 1-pixel sun-arc indicator along the top edge driven from it.
  - **Win:** swapping from `clock` to `moon_phase` shows the moon disc
    immediately on the first frame (no stall); the chrome arc visibly
    marches across the day in `sky_timelapse`.

- [ ] **D.7 Speculative `Scene::prepare()`** (FR-16.4)
  - Add an optional `Scene::prepare(now_ms)` hook (default no-op).
    During Core 1 slack windows (gated by D.5 floor), call `prepare()`
    on the most likely next scene — heuristic: if there's a pending
    request, prep that; else prep the default. `ConstellationNow`
    pre-packs the next constellation's line-art bitmap;
    `ImagePaletteBg` pre-builds its themed runtime palette.
  - **Win:** instrument the first-frame render time after a swap;
    pre-prepared scenes show a measurable drop (e.g. `ConstellationNow`
    first frame goes from ~25 ms to < 10 ms). FPS uninterrupted.

- [ ] **D.8 Toast / banner overlay** (FR-16.6)
  - Add `observatory/toast` topic, payload `{"text": "...", "ms": N,
    "priority": P}`. Core 0 validates (FR-1.4 pattern), pushes into a
    small SPSC ring (capacity 3). Core 1's compositor picks up new
    toasts each frame and installs them as a `LAYER_OVERLAY_TRANSITION`
    layer with a bounded lifetime, drawn in `theme::ink(INK_ALERT)`
    with halo and a slide-in/fade-out envelope. Stacks up to 3; oldest
    expires first.
  - **Win:** `mosquitto_pub -t observatory/toast -m '{"text":"ISS NOW",
    "ms":3000}'` flashes a banner over whatever scene is active without
    interrupting it.

- [ ] **D.9 Link-health "breathing" chrome dot** (FR-16.8)
  - Chrome layer reads Core 0's MQTT keep-alive timestamp, renders a
    ≤ 2 px dot in a fixed corner with a slow brightness sin-wave when
    fresh, fades to dim when stale (> 5 s), and disappears entirely
    when the OFFLINE overlay has engaged (avoids redundancy). Themed
    via `theme::ink(INK_OK)`.
  - **Win:** unplug the router → the dot fades over ~5 s before the
    OFFLINE overlay (FR-5.1) takes over. Plug back in → the dot
    brightens immediately, well before the next status heartbeat.

- [ ] **D.10 Compositor coverage in `gfx_test`** (FR-16.10)
  - Extend the diagnostic scene to cycle, on a fixed cadence, every
    overlay (toast, night, thermal, offline) and the crossfade
    transition, so one capture validates the entire compositor path.
    Print per-layer render times and slack to serial.
  - **Win:** running `gfx_test` for 30 s exercises every layer; serial
    log shows a per-layer timing table; visual regressions in any
    layer are caught with one MQTT command.

---

## Phase 9 — Hardening (final)

- [ ] **9.1 Memory audit** — log free heap; confirm ≥ 32 KB headroom under all scenes; also flash budget — each `assets/*.bmp` costs ~2.4 KB; track total registry size
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

# Quantum Observatory — Requirements

**Version:** 1.8
**Status:** Draft — implementation in progress (W1–W5 landed; FR-12 color/background system landed; FR-13 splash + sky background landed; FR-14 on-device astronomical computation landed for ISS/sun/moon; FR-15 theming system in progress (T.1–T.2 landed); FR-16 compositor in progress (D.1–D.2 landed))
**Target hardware:** Raspberry Pi Pico W + Waveshare RGB-Matrix-P3 (64×32, FM6126A driver, HUB75)
**Stack:** C++ on PlatformIO (earlephilhower Arduino-Pico core), Adafruit Protomatter, MQTT client, Home Assistant integration

---

## 1. Purpose & Scope

A network-connected 64×32 RGB matrix display that renders curated astronomical and ambient scenes. Home Assistant owns all data/logic ("Director"); the Pico W owns rendering ("Cinematographer"). Communication is stateless and intent-based — no raw pixel streaming.

The device also doubles as **the only clock in the room**. The current time MUST be visible at all times — every scene carries a small clock readout, and a dedicated giant-clock scene serves as the default "nothing else to show" view.

**Audience:** the dashboard targets teenagers — roughly 80% astronomy interest (planets, the Moon, ISS passes, what's visible tonight) and 20% astrophysics interest (scale, orbits, light-years, deep-sky context). Scene copy, scene selection, and visual priorities SHALL favour the observable-sky framing first; astrophysics framing is a flavor layer, not the headline.

**In scope:** scene rendering engine, MQTT scene contract, dual-core orchestration, OTA scene registry, offline fallback, always-on clock readout, dedicated clock/date scene.
**Out of scope:** raw pixel streaming, server-side layout, touchscreen/input handling, audio, weather/meteorological alerts (this is an *astronomy* dashboard; HA already surfaces weather elsewhere in the home).

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
- **FR-3.1** The device SHALL maintain a target frame rate of **≥ 24 FPS** sustained during normal operation, with a per-frame budget aligned to a fixed `kFrameIntervalMs` (~41 ms).
- **FR-3.2** The renderer SHALL composite an ordered layer stack — `[background] [scene foreground] [overlays] [chrome]` — every frame. Each layer owns its own animation clock. Detailed contract in FR-16.1; this clause is the high-level statement.
- **FR-3.3** Text SHALL be drawn with a destructive halo / bounding box to remain legible over animated backgrounds.
- **FR-3.4** The device SHALL support at minimum the following ambient backgrounds: 3-level parallax starfield, animated nebula (palette-cycled), static deep-sky starfield, palette-indexed bitmap (procedural), palette-indexed image (artist-supplied .bmp), and sun-aware sky gradient (FR-13.2).
- **FR-3.5** The device SHALL support at minimum the following transitions: **instant cut** and **fade-through-black** (FR-16.3, v1 default). Additional transitions (dissolve, warp, true crossfade) are reserved for future work once the off-screen rendering plumbing exists; they are not v1 requirements.
- **FR-3.6** The renderer SHALL provide a `gfx_test` diagnostic scene (FR-12.6, FR-15.8) that exercises the smooth-gradient path, palette cycling, every theme ink/hint role, and the compositor overlay/transition layers (FR-16.10), and reports live FPS so visual regressions can be caught with one MQTT command.

### FR-4 Typography
- **FR-4.1** Font selection SHALL be owned by the active theme (FR-15) — scenes resolve fonts via `theme::font(role)`, never by direct `setFont(&...)`. The default theme (`apollo_amber`) SHALL ship with a compact 5×7 data font (`Silkscreen` or equivalent) and a bold header font (`Press Start 2P` or equivalent compact bold); other themes substitute as defined in [THEME.md](THEME.md).
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
- **FR-7.5** Mode priority (highest preempts lower): `thermal_safe` > `night` > MQTT-requested scene > default. These modes are firmware-owned safety/ambient overrides and bypass the FR-2 Director priority field. Per FR-16.2, they SHALL be implemented as compositor overlays (not scene preemptions) so the underlying scene continues to animate behind them; the priority ordering above governs which overlay wins when multiple are simultaneously active.
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
- **FR-10.1** The firmware SHALL drive the on-board buzzer (GP27, active-high) for short attention chirps tied to high-priority scenes (priority ≥ 4, e.g. `iss_pass`).
- **FR-10.2** Buzzer behaviour (mute / chirp pattern) SHALL be remotely controllable via MQTT (e.g. `observatory/buzzer`, payload = `{"mode": "off"|"chirp"|"siren", "count": N}`). HA owns the policy; firmware owns the timing.
- **FR-10.3** A firmware mute switch SHALL hard-cap any chirp to ≤ 200 ms ON / ≥ 800 ms OFF and ≤ 3 chirps per scene activation, regardless of MQTT command, to avoid runaway noise from a malformed payload.
- **FR-10.4** The buzzer SHALL default to off after boot. A boot-time self-test chirp is allowed but MUST be ≤ 50 ms.

### FR-11 Local Input (on-board buttons)
- **FR-11.1** The firmware SHALL read the three on-board buttons (GP15 = MENU, GP19 = DOWN, GP21 = UP) with debounce ≥ 30 ms.
- **FR-11.2** When MQTT is connected, button presses SHALL be published to HA (e.g. `observatory/button`, payload = `"menu"|"down"|"up"`) so HA can drive scene response. Local behaviour SHALL be a no-op in this mode (Director still owns intent).
- **FR-11.3** When MQTT is disconnected, the buttons SHALL provide a minimal local fallback: MENU cycles between `clock` and `offline` scenes; UP/DOWN reserved for future local actions (no firmware-managed brightness in v1).

### FR-12 Color & Background System
- **FR-12.1** The renderer SHALL use a split-layout palette table to deliver smooth gradients at the panel's 5-bit-per-channel depth. Every palette is exactly 256 RGB565 entries divided as **0..191 = background region (cyclic)** and **192..255 = foreground region (linear brightness ramp)**. (The split is documented at the API surface as `palette::BG_LEN = 192`, `palette::FG_BASE = 192`, `palette::FG_LEN = 64`.)
- **FR-12.2** Palettes SHALL be built once at boot from compact stop-list definitions (linear interpolation between stops, round-to-nearest RGB565 packing), and read-only thereafter. No per-frame palette computation.
- **FR-12.3** Background regions SHALL support **palette cycling** without re-computing pixels: a renderer animates by walking a per-frame shift index modulo 192. A background MAY define multiple disjoint sub-ranges (`Region {start, length, speed}`, max 4), each cycling at its own signed speed, so different parts of one image can flow at different rates and directions.
- **FR-12.4** The renderer SHALL support two background flavors:
  - **Dynamic** — computes pixels every frame (e.g. starfield, parallax, nebula).
  - **Palette-indexed bitmap** — a 64×32 array of 1-byte palette indices (BG region only, indices 0..191) plus a per-image 192-entry palette and region table. Pixels are computed once at init (procedural) or imported from artist-supplied artwork (FR-12.5); cycling is then "free."
- **FR-12.5** The build system SHALL include a pre-build asset import pipeline that converts `assets/*.bmp` (8-bit indexed, uncompressed BMP, exactly 64×32) into flash-resident `constexpr` headers under `include/bitmaps/`. The converter SHALL hard-fail the build if any pixel references a palette entry ≥ 192, preserving the FG-region invariant. An optional `assets/<name>.regions` sidecar text file MAY declare cycling sub-ranges (`start length speed` per line); when absent, a single static region is emitted (no animation).
- **FR-12.6** The firmware SHALL expose a `gfx_test` scene that simultaneously demonstrates: (a) a cycling background gradient using a multi-stop BG palette (smoothness + seamless 191→20 wrap), (b) three full-width foreground brightness ramps (white / blue / amber) confirming low-end smoothness in all three star palettes, (c) a live 1-second-window FPS counter and uptime/shift readouts, and (d) a single-pixel "jitter witness" hopping each frame so frame-rate irregularity is visible by eye. The scene SHALL be selectable over MQTT via `scene_id: "gfx_test"` like any other scene.
- **FR-12.7** Authoring constraints for `assets/*.bmp` SHALL be documented at `assets/README.md` (format, dimensions, indexed-only requirement, optional regions sidecar). The 192-index ceiling is non-negotiable: it is the contractual boundary between cycling-eligible and reserved-for-foreground palette entries.
- **FR-12.8** The asset import pipeline (FR-12.5) SHALL apply gamma correction (γ = 2.2) when packing 8-bit RGB triples to RGB565, so artist-supplied artwork looks perceptually correct on the panel's non-linear LED response.

### FR-13 Boot Splash & Sky Background
- **FR-13.1** The firmware SHALL present a boot splash (`assets/observatory.bmp` rendered via FR-12.4 image background) from power-on until the first successful MQTT connect. The splash SHALL preempt every other scene and overlay including `night` and `thermal_safe` (highest-priority firmware override). Per FR-16.2 it is implemented as a compositor overlay above all other safety overlays. Once cleared by the first connect, it SHALL be latched and never re-shown by subsequent disconnects.
- **FR-13.2** The firmware SHALL provide a `sky` background that renders a sun-aware sky gradient driven by the live RTC time and a hardcoded observer latitude/longitude (Houston in v1; future MQTT-settable). Sun position SHALL use a low-precision NOAA solar model (±1°). The gradient SHALL select between five altitude bands (day / golden hour / civil / nautical / astronomical twilight) and interpolate top→bottom across the panel.
- **FR-13.3** The sky background SHALL render the sun as a multi-tier disc (~9 px) with a vertical color gradient (pale on top, warm on bottom) that intensifies near the horizon to mimic atmospheric reddening. The sun's screen position SHALL trace a semi-elliptical arc using a linear azimuth→x mapping, clamped so the disc is never cropped at the panel edges.
- **FR-13.4** The firmware SHALL provide a `sky_timelapse` debug scene that compresses one synthetic day into 10 seconds to allow visual validation of the FR-13.2/13.3 sky and sun rendering without waiting for real-time motion. It SHALL be selectable via the standard MQTT scene contract (FR-2).
- **FR-13.5** The DS3231 read path (FR-9.5) SHALL include integrity defenses against transient I²C corruption: internal pull-ups via `gpio_pull_up()` (never `pinMode(INPUT_PULLUP)`, which breaks the I²C alt-function), per-burst sanity-clamp of BCD fields, two-burst consensus read (accept only if Δseconds ∈ [0,1]), and a poll-validated outer cadence that rejects any RTC value diverging from the projected time by more than 3 hours. Successful polls back off to a 1-hour cadence; rejected polls retry with exponential backoff capped at 1 hour.

### FR-14 On-Device Astronomical Computation

The Director (HA) is authoritative for *event* triggering (e.g. "ISS pass starts at 21:04:11 with max altitude 78°"), but several scenes need to evolve their visuals continuously between events without round-tripping HA every frame. Those derivations run on the device.

- **FR-14.1** Sun position (altitude/azimuth) SHALL be computed on-device from the RTC time and a hardcoded observer latitude/longitude (Houston in v1; future MQTT-settable) using a low-precision NOAA solar model (±1°). This is the source for FR-13.2/13.3 and the sky-model snapshot in FR-16.5.
- **FR-14.2** ISS look-angle (altitude/azimuth/range) and visibility classification (visible / daylight / shadow) SHALL be derived on-device for the duration of an active pass, given a pass envelope pushed by HA at pass start (TLE-derived rise/set times, max-altitude azimuth, crew count). The `iss_pass` scene consumes this derivation; HA is not polled per frame.
- **FR-14.3** Jupiter visibility classification (above-horizon-and-dark / above-horizon-but-daylight / below-horizon) SHALL be derived on-device from Jupiter's RA/Dec (pushed by HA at low cadence — daily is sufficient), the RTC time, and the observer location. The `jupiter_visibility` scene consumes this derivation to decide between the headline framings ("Visible: East @ 9PM" vs. "Behind the Sun" vs. host constellation when above-horizon-but-daylight).
- **FR-14.4** Moon phase (illuminated fraction, waxing/waning) SHALL be computed on-device from the RTC time using a closed-form approximation (≤ ±2% phase error). The `moon_phase` scene consumes this derivation; HA pushes only the moon's RA/Dec for altitude calculation when the scene needs "rise/set tonight" framing.
- **FR-14.5** All FR-14 computations SHALL conform to NFR-1.3 (fixed-point math and/or precomputed LUTs; no software-emulated `float` in render loops). Trig and ephemeris work runs on Core 1 during slack windows (FR-16.5) and is published via the seqlock snapshot (FR-16.7).

### FR-15 Theming System
Full design lives in [THEME.md](THEME.md); these are the contractual bullets.

- **FR-15.1** The firmware SHALL ship at least five named themes drawn from canonical retro sci-fi reference points: `apollo_amber` (default), `nostromo_green`, `vectrex_neon`, `blade_runner`, `lcars_tos`. Each theme SHALL bundle its own ink palette, font selection, bracket convention, and layout hints — themes are not color-only swaps.
- **FR-15.2** The active theme SHALL be selectable via MQTT topic `observatory/theme`, payload `{"id": "<theme_id>"}` (string id). Unknown ids SHALL be ignored and logged (FR-1.3 spirit). The firmware SHALL boot to `apollo_amber` and accept the Director's choice on connect; the active theme is not persisted across reboots (no flash wear).
- **FR-15.3** Scenes SHALL NOT hardcode ink colors, font selections, or bracket strings. All theme-affected rendering SHALL go through a `theme::*` API that resolves the active theme on every call. CI / code review SHALL flag literal RGB565 constants and `setFont(&...)` calls inside `src/scenes/`.
- **FR-15.4** A theme switch SHALL take effect at the next-frame boundary with no torn frames and no scene re-init. The active scene SHALL continue rendering, simply consulting the new theme's values starting from the next `render()` call.
- **FR-15.5** The firmware-owned safety overrides (`THERMAL_SAFE`, `NIGHT`, `OFFLINE`, `SPLASH`) SHALL preserve the active theme — they affect brightness and message, not theme identity. The dim variants of these scenes SHALL consult `theme::ink(...)` and render at the low end of the FG ramp.
- **FR-15.6** Background renderers (`STARFIELD`, `PARALLAX`, `NEBULA`, `BITMAP`, `IMAGE`) SHALL consult `theme::bg_palette_for(...)` for the active palette, never a hardcoded `palette::Id`. The default theme (`apollo_amber`) SHALL pass through each image's original baked palette so the as-shipped look is preserved bit-for-bit. Non-default themes SHALL declare `BG_HIGHLIGHT` and `BG_SHADOW` (with optional `BG_BLACK` / `BG_WHITE` anchor overrides) and the firmware SHALL synthesize a per-image runtime palette at theme-switch time by mapping each source-palette entry's luminance (BT.601, build-time-precomputed and histogram-stretched per image) through the 4-stop ramp `BG_BLACK → BG_SHADOW → BG_HIGHLIGHT → BG_WHITE`. Runtime palettes SHALL be double-buffered with an atomic active-index byte to avoid torn frames during the rebuild. Theme-switch wall time (MQTT message arrival to first frame in new theme) SHALL be ≤ 5 ms for the v1 asset set. Individual images MAY opt out via an `assets/<name>.notheme` sidecar (empty file); opted-out images render in their original palette under every theme. The asset import pipeline (FR-12.5) SHALL emit a `uint8_t lum[192]` luminance table next to each themable image's palette and SHALL set the registry's `themeable` flag from the sidecar's presence.
- **FR-15.7** The `observatory/status` heartbeat (§5.4) SHALL include the active theme id so HA can confirm the device's state without round-tripping the theme topic.
- **FR-15.8** The `gfx_test` scene (FR-12.6) SHALL be extended to exercise every `theme::Ink` role and every layout `Hint` so visual regressions in the theming layer are caught with one MQTT command. Theme cycling within `gfx_test` SHALL be on a fixed cadence so a single capture covers all themes.

### FR-16 Dual-Core Compositor & Idle-Slack Utilization

The Phase 4 dual-core split (FR §4.1) treats Core 1 as a single-scene
renderer that frame-caps at ~24 FPS and idles between `show()` calls.
That residual headroom is a first-class resource. FR-16 graduates Core 1
from "render the active scene" to "compose layers, simulate the sky,
and pre-stage the next scene", and tightens the cross-core data path
so Core 0's network jitter cannot perturb the frame.

- **FR-16.1 Layered compositor.** The render pipeline (NFR §4.3) SHALL be
  reframed as an ordered layer stack — `[background] [scene foreground]
  [overlays...] [chrome]` — composited every frame. Each layer SHALL own
  its own animation clock and SHALL NOT depend on a "current scene"
  re-init to advance. Adding a layer SHALL NOT require touching the
  scene dispatcher or `scene_for()`.

- **FR-16.2 Safety overrides as overlays.** `NIGHT`, `OFFLINE`,
  `THERMAL_SAFE`, and `SPLASH` SHALL be implemented as compositor
  overlays (with fade-in / fade-out envelopes) layered on top of the
  underlying scene, NOT as full scene preemptions. The underlying scene
  SHALL continue to render and animate behind the overlay so that on
  override clear, the user sees a fade rather than a scene restart
  (e.g. `ConstellationNow`'s slow reveal SHALL NOT restart when NIGHT
  lifts). The dispatcher's "active scene" SHALL remain the
  Director-requested or default scene at all times; only the overlay
  bit changes. FR-7.5 priority semantics (which overlay wins when two
  are simultaneously active) are preserved.

- **FR-16.3 Scene fade-through-black transitions.** Scene swaps via
  `take_pending()` SHALL render through a fade-through-black transition
  (~250 ms total, integer-only alpha, ~24 FPS): the *outgoing* scene
  fades to black over the first half, then the *incoming* scene fades
  in from black over the second half. The dispatcher swaps
  `g_current_scene` exactly once at the half-way point; only one scene
  renders per frame. No off-screen scratch buffers, no per-pixel cross
  blend between two simultaneously-rendered scenes — this is the
  v1 transition (chosen for memory and signature simplicity over a true
  crossfade). The fade SHALL be implemented as a black overlay layer in
  `LAYER_OVERLAY_TRANSITION` running an **8×8 ordered Bayer dither**:
  each frame the layer walks the panel and over-writes pixels with
  `0x0000` wherever `bayer8[x%8][y%8] < alpha_threshold` (alpha ramped
  0..255 via integer math). This requires no framebuffer readback and
  no `Scene::render` signature change — it draws on top of whatever
  the active scene + chrome left in the live framebuffer. Hard-cut
  SHALL remain available as a transition type for cases where the
  fade-through-black aesthetic is not wanted (FR-16.4 fallback).
  Future requirements MAY introduce additional transition types (true
  crossfade, warp, dissolve per FR-3.5) once the off-screen rendering
  plumbing exists.

- **FR-16.4 Speculative pre-render.** During the frame-cap idle window
  on Core 1, the renderer SHALL invoke a `Scene::prepare(uint32_t now_ms)`
  hook on the most likely next scene (heuristic: highest-priority pending
  request, else default). `prepare()` SHALL be idempotent and SHALL NOT
  draw to the live framebuffer; its purpose is to amortize one-shot work
  (constellation line packing, image palette LUT rebuilds, sky-model
  pre-roll) so that the first post-swap frame is not visibly slower than
  steady-state. Scenes that cannot benefit from pre-render SHALL leave
  `prepare()` at its default no-op.

- **FR-16.5 Continuous sky-model on Core 1.** A 1 Hz background sky
  simulation SHALL run on Core 1 between frames, computing sun
  altitude/azimuth (per FR-13.2/13.3), moon phase + altitude, and ISS
  position regardless of which scene is active. Results SHALL be
  published into a shared snapshot readable by any scene and by the
  chrome layer. Rationale: scene swaps to sky-aware scenes become
  instant (no first-frame stall), and the chrome layer can carry
  ambient micro-indicators (e.g. a 1-pixel sun arc along the top edge
  showing day progress, a moon-phase pip in the corner) without
  burdening Core 0 alongside MQTT.

- **FR-16.6 Toast / banner overlays.** The compositor SHALL support
  ephemeral overlay layers ("toasts") with a bounded lifetime (≤ 10 s),
  triggerable by Core 0 from MQTT (e.g. `observatory/toast`, payload
  `{"text": "...", "ms": 3000, "priority": N}`) without inventing a new
  scene. Toasts SHALL stack up to a small fixed cap (≤ 3) and SHALL
  preserve the active scene underneath. Out of scope for v1: toast
  styling beyond the active theme's `INK_ALERT`.

- **FR-16.7 Seqlock cross-core snapshots.** The per-frame read path
  (Core 1 reading scene state + sky-model snapshot + sensor flags) SHALL
  use a seqlock-style sequence-counter pattern, NOT a `mutex_t`, for
  read-mostly state. Writers (Core 0) SHALL increment an odd seq before
  write and an even seq after; readers (Core 1) SHALL retry on torn
  reads. This generalizes the pattern already in use for
  `g_render_alive_ms` / `g_render_fps`. Rationale: Core 1 SHALL never
  block on Core 0's network jitter during a frame. The existing
  `mutex_t` SHALL remain for write-write coordination on Core 0.

- **FR-16.8 Cross-core link-health indicator.** Core 1's compositor
  SHALL render a continuous low-amplitude "breathing" indicator in
  the chrome layer driven by Core 0's MQTT keep-alive timestamp,
  giving the user ambient confirmation that the link is healthy
  without firing the FR-5.1 OFFLINE override. The indicator SHALL be
  ≤ 2 px, themed via `theme::ink(INK_OK)`, and SHALL fade to dim when
  the keep-alive is stale (> 5 s) before the OFFLINE overlay engages.

- **FR-16.9 Idle-slack budget & instrumentation.** Core 1 SHALL track
  per-frame slack (kFrameIntervalMs minus actual render time) and
  publish a rolling average to Core 0 for inclusion in
  `observatory/status` (`render_slack_ms`). FR-16.4 / FR-16.5 work
  SHALL only execute when slack ≥ a configurable floor (default 8 ms)
  to preserve FR-3.1's frame-rate target under load. Heavy scenes
  SHALL be allowed to spend the full frame budget on themselves.

- **FR-16.10 Backwards compatibility.** The single-pointer `g_current_scene`
  model SHALL continue to work for scenes that do not opt into the
  compositor's per-layer hooks. The Scene Contract (§5.1) is unchanged.
  Migration of NIGHT/OFFLINE/THERMAL/SPLASH from preemption to overlay
  (FR-16.2) is the only behavioural change visible at the MQTT surface,
  and only for the case where an override clears mid-scene.

### FR-17 IR Remote Input

The carrier board ships a 38 kHz IR demodulator on GP28 (silkscreen
"IRM"). FR-17 graduates the phase IR.1 logging-only POC into a
first-class operator interface, while preserving HA as the source of
truth for scene scheduling. The interaction model is **hybrid**: a
local fast path for viewer-ergonomics actions (theme switch, info
overlay, splash dismiss) so the device stays useful when MQTT is down,
plus an MQTT round-trip for Director-class intent (jump-to-specific-
scene) so HA's history and automations stay authoritative.

- **FR-17.1 Receiver binding.** The firmware SHALL bind the IR receiver
  on Core 0 (Gatekeeper). The decoder's pin-change ISR + microsecond
  timer SHALL NOT run on Core 1 — Core 1 owns Protomatter timing and
  cannot tolerate ISR jitter mid-frame (FR-3.1, NFR-1.2).

- **FR-17.2 Decoder discipline.** Decoded frames SHALL be acted upon
  only when `protocol == NEC`, `flags & PARITY_FAILED == 0`, and
  `flags & WAS_OVERFLOW == 0`. All other decodes (UNKNOWN, parity
  failures, overflows) SHALL be counted for diagnostics and discarded.
  This is the contract that defends the dispatch table against
  HUB75-EMI ghost decodes.

- **FR-17.3 Per-remote address gate.** The firmware SHALL ignore NEC
  frames whose address byte does not match a configured expected
  address (`config.h::IR_REMOTE_ADDR_EXPECTED`, captured from the
  intended remote during phase IR.2). This stops a neighbour's TV
  remote from accidentally driving the panel.

- **FR-17.4 Repeat-frame policy.** NEC repeat frames
  (`flags & IS_REPEAT`) SHALL be honoured or ignored on a per-action
  basis declared in the dispatch table, never globally. Actions that
  ramp continuously (none in v1) MAY honour repeats; actions that
  step state discretely (theme cycle, scene cycle, info overlay)
  SHALL ignore repeats so a long-press does not stampede.

- **FR-17.5 Hybrid lane assignment.** Each mapped button SHALL be
  classified as either **local-fast** (acts on local state directly
  on Core 0) or **mqtt-routed** (publishes to `observatory/remote/event`
  and lets HA decide). The v1 mapping for the Roku-style 8-button
  remote SHALL be:

  | Button | Lane | Action |
  |---|---|---|
  | `▲` / `▼` | local-fast | scene cycle (next / prev in `kRemoteCycle[]`) |
  | `◄` / `►` | local-fast | theme cycle (prev / next theme, FR-15.1) |
  | `OK` | local-fast | toggle info overlay (5 s, FR-17.8) |
  | `Back` | local-fast | clear sticky + return to default `CLOCK`; also dismiss splash if active |
  | `Home` | local-fast | force default `CLOCK` immediately (no sticky clear) |
  | `*` (Options) | mqtt-routed | publish `{"button":"options"}` — HA-defined behaviour |
  | `↺` (Replay) | mqtt-routed | publish `{"button":"replay"}` — HA-defined behaviour |
  | streaming-service shortcuts | mqtt-routed | publish `{"button":"<svc>"}` if remote emits unique IR |

  Local-fast actions SHALL also publish an echo to
  `observatory/remote/event` (FR-17.7) for observability, but SHALL NOT
  block on the publish.

- **FR-17.6 Curated cycle list.** Scene cycle (`▲`/`▼`) SHALL walk a
  fixed list of operator-facing scenes declared in `config.h`
  (`kRemoteCycle[]`). The list SHALL exclude all firmware-owned
  override scenes (`BOOT`, `NIGHT`, `THERMAL_SAFE`, `OFFLINE`, `SPLASH`)
  and the diagnostic scenes (`GFX_TEST`, `IR_TEST`) by default.
  Cycle requests SHALL use `priority=1` and `duration=120s` so a real
  ISS pass (priority 4) can still preempt.

- **FR-17.7 MQTT echo.** Every accepted IR press SHALL publish to
  `observatory/remote/event` (§5.5) with `{button, action, accepted,
  protocol, address, command}`. Discarded frames (FR-17.2 / FR-17.3)
  SHALL NOT publish; they SHALL only update the diagnostic counters
  surfaced by `IrTestScene`.

- **FR-17.8 Info overlay.** The firmware SHALL provide an
  `InfoOverlayLayer` compositor layer (FR-16.1 slot above SCENE,
  below OVERLAY_TRANSITION) that fades in on `OK` press and out
  after 5 s. Content SHALL include: IP address, RSSI, MQTT link
  state, uptime, FPS, current scene id, current theme id, free heap.
  A second `OK` press while visible SHALL dismiss it immediately.

- **FR-17.9 Visual feedback.** Every accepted IR press SHALL produce a
  visible change within one frame (≤ 1/24 s):
  - Scene cycle / theme cycle / overlay toggle / dismiss — the action
    itself is the feedback.
  - MQTT-routed presses — a single chrome-row pixel SHALL flash green
    on accept, or red if MQTT is currently disconnected (the press is
    queued for retry per the publisher's existing semantics).

- **FR-17.10 Theme persistence.** Theme cycle (`◄`/`►`) SHALL invoke
  the same `theme::set()` path that MQTT uses (FR-15.2, FR-15.4) so
  the next-frame swap and the status-heartbeat echo (FR-15.7) are
  preserved. The remote-driven theme is NOT persisted across reboots
  (NFR-3 — no flash wear); on boot the device SHALL return to
  `apollo_amber` until either MQTT or the remote pushes a choice.

- **FR-17.11 IR vs. on-board buttons.** FR-17 SHALL coexist with
  FR-11 (on-board buttons) without overlap: on-board buttons remain
  as documented; the IR mapping is independent. Both input paths
  publish to topic-distinct echoes (`observatory/button` for the
  on-board buttons per FR-11.2, `observatory/remote/event` for IR per
  FR-17.7) so HA can distinguish them.

- **FR-17.12 Diagnostic scene.** The `ir_test` scene (phase IR.1)
  SHALL remain available in v1 as the on-panel diagnostic for
  receiver behaviour and EMI characterisation. It SHALL NOT be in
  the curated cycle list (FR-17.6) and SHALL be reachable only via
  explicit MQTT request.

- **FR-17.13 EMI tolerance.** The end-to-end IR system (receiver +
  decoder + dispatch) SHALL accept a deliberate user press with
  ≥ 90% reliability under the brightest production scene at full
  brightness, measured over 30 presses. Failure to meet this bar
  SHALL escalate to a hardware mitigation (LC filter on receiver
  Vcc, ferrite bead on signal line, or physical shielding) before
  FR-17 is declared complete.

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
- **NFR-5.3** Adding a new artist-supplied background image SHALL require only dropping `assets/<name>.bmp` (and optionally `<name>.regions`) into the repo. The asset import pipeline (FR-12.5) SHALL pick it up on the next build with no source-code edits.

---

## 4. Architecture

### 4.1 Dual-Core Split

| Core | Role | Responsibilities |
|---|---|---|
| **Core 0 — Gatekeeper** | Network & state | Wi-Fi mgmt, MQTT pub/sub, JSON parsing, Scene Registry lookup, writes to shared `SceneState` struct, watchdog feed |
| **Core 1 — Artist & Compositor** | Rendering, layer composition, sky simulation, speculative pre-render | Reads `SceneState` (seqlock-snapshot per FR-16.7), composes the layer stack (FR-16.1), drives the 1 Hz sky model (FR-16.5), runs `Scene::prepare()` for the likely next scene during slack windows (FR-16.4), drives Protomatter, maintains FPS |

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
{ "scene_id": "...", "fps": 28, "rssi": -55, "uptime_s": 1234, "free_heap": 180000, "render_slack_ms": 21, "theme": "apollo_amber" }
```

`render_slack_ms` is the rolling average per-frame idle window on Core 1
(FR-16.9); HA can use it as a budget gauge for adding new layers / heavier
scenes. `theme` is the active theme id (FR-15.7).

### 5.5 Remote Event (Pico → HA, FR-17.7)
Topic: `observatory/remote/event` — published once per accepted IR press:
```json
{ "button": "up", "action": "scene_cycle_next", "accepted": true, "lane": "local",
  "protocol": 8, "address": 85, "command": 10 }
```

| Field | Type | Notes |
|---|---|---|
| `button` | string | Logical name from FR-17.5 mapping (`up`, `down`, `left`, `right`, `ok`, `back`, `home`, `options`, `replay`, ...) |
| `action` | string | Resolved action (`scene_cycle_next`, `theme_cycle_prev`, `info_overlay_toggle`, `mqtt_passthrough`, ...) |
| `accepted` | bool | False only if dispatch table rejected the press (e.g. unmapped command id from a known-but-partial remote model) |
| `lane` | string | `"local"` (FR-17.5 local-fast) or `"mqtt"` (HA-defined behaviour) |
| `protocol` | int | `decode_type_t` value (8 = NEC) |
| `address` | int | NEC address byte (FR-17.3 expected-address gate has already passed) |
| `command` | int | NEC command byte |

Discarded frames (FR-17.2 wrong protocol / parity / overflow, FR-17.3
wrong address) SHALL NOT be published; they're only counted by the
diagnostic in `IrTestScene`.

---

## 6. Initial Scene Registry (target set for v1.0)

| scene_id | bg_type | text_layout | notes |
|---|---|---|---|
| `boot` | starfield | "OBS" centered | shown at startup |
| `splash` | image (observatory.bmp) | — | firmware override; compositor overlay (FR-16.2), shown until first MQTT connect (FR-13.1) |
| `clock` | sky | giant HH:MM + date line | default / idle scene (FR-9.4); sky bg per FR-13.2 |
| `offline` | starfield_dim | local time | MQTT disconnect fallback (FR-5.1); compositor overlay (FR-16.2) |
| `night` | black | dim HH:MM only | LDR-triggered (FR-7.2); compositor overlay (FR-16.2), preempts MQTT scenes |
| `thermal_safe` | black | dim "COOL DOWN" + temperature | DS3231-triggered (FR-7.3); compositor overlay (FR-16.2), preempts everything except `splash` |
| `bg_starfield` | static deep-sky | — | static field + small twinkle overlay |
| `bg_parallax` | parallax | — | 3-level scrolling stars |
| `bg_nebula` | nebula | — | dynamic palette-cycled clouds (FR-12.3/12.4) |
| `bg_bitmap` | bitmap | — | procedural palette-indexed bitmap demo |
| `bg_image` | image | — | first artist-supplied `assets/*.bmp` (FR-12.5) |
| `gfx_test` | gradient + ramps | live FPS readout | diagnostic (FR-12.6) |
| `sky_timelapse` | sky (synthetic time) | "TIMELAPSE" label | diagnostic (FR-13.4); 1 day per 10 s |
| `iss_pass` | nebula | typewriter ALT/CREW/VIS | priority 4; on-device look-angle + visibility derivation per FR-14 |
| `moon_phase` | starfield | phase glyph + name | sticky |
| `jupiter_visibility` | nebula | direction + magnitude/distance + visibility (or host constellation when above-horizon-but-daylight) | example in §5.1; on-device daylight derivation per FR-14 |
| `constellation_now` | starfield | constellation art + name | sticky; HA picks current overhead constellation by date + observer lat/lon — see [FUTURE_SCENES.md](FUTURE_SCENES.md) Tier 1 |

All scenes above (except possibly `boot` during the splash window) carry the standard small clock readout per FR-9.2. All `bg_type` values in this table are subject to FR-15.6 — the actual palette used at render time is whatever `theme::bg_palette_for()` returns for the active theme.

---

## 7. Phased Roadmap

The high-level roadmap below is intentionally coarse; the detailed,
check-the-box implementation plan lives in [PLAN.md](PLAN.md) (Phases 0–9
plus Phase T theming and Phase D compositor).

| Wave | Theme | PLAN.md phases |
|---|---|---|
| **W1 — Foundation & render plumbing** | Single-core scene engine, fonts, clock substrate, RTC | Phase 0–3.6 |
| **W2 — Color & background system** | Split-palette LUT, BMP pipeline, gfx_test diagnostic | Phase 3.7 |
| **W3 — Network MVP** | Dual-core split, Wi-Fi + MQTT, scene contract, night/thermal, RTC correction | Phase 4–5 |
| **W4 — Scene lifecycle & resilience** | Priority preemption, TTL, sticky, offline, watchdog, splash, sky bg | Phase 6–6.5 |
| **W5 — First real observatory scenes** | iss_pass, moon_phase, jupiter_visibility, constellation_now | Phase 7 |
| **W6 — Theming system** | Five themes, MQTT-selectable, runtime BG duotone | Phase T |
| **W7 — Compositor & idle-slack utilization** | Layer stack, fade transitions, overlays, sky-model on Core 1, toasts | Phase D |
| **W8 — Polish & hardening** | OTA, registry-as-data, soak test, tag v1.0 | Phase 8–9 |

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
10. **Image selection over MQTT** (FR-12.5): today `bg_image` always shows the first registered `assets/*.bmp`. When the asset library grows past 1, do we extend the Scene Contract with an `image_id` override, add per-image scene IDs, or expose a separate `observatory/image` topic? Defer until the second image lands.
11. **Runtime asset upload** (FR-12.5 extension): currently images are flash-resident at compile time. Should we support pushing new images over MQTT into LittleFS so HA can refresh artwork without a reflash? (Tied to OQ #2 and the OTA strategy.)

---

## 10. Glossary

- **Director**: Home Assistant — owns data, scheduling, and intent.
- **Cinematographer**: Pico W firmware — owns rendering and timing.
- **Scene**: a self-contained visual program identified by `scene_id`.
- **Scene Contract**: the JSON schema in §5.1.
- **Sticky**: a scene that does not auto-expire on `duration`.
- **TTL**: hard maximum lifetime for any scene (default 1 hour).
- **Destructive Overlay**: drawing technique where text writes opaque halo pixels into the background layer to guarantee legibility.
- **Theme**: a bundle of inks, fonts, brackets, and layout hints that gives the device a distinct retro sci-fi identity (Apollo MOCR, Nostromo CRT, Vectrex, Blade Runner, LCARS). Selectable over MQTT (FR-15).
- **Compositor**: Core 1's per-frame layer-stack pipeline (FR-16.1). Replaces the single-scene render model with an ordered `[bg][fg][overlays][chrome]` stack so safety overrides, toasts, and chrome micro-indicators stack additively without scene re-init.
- **Overlay**: a compositor layer drawn over the active scene, owned by the firmware (FR-16.2 safety overrides) or by Core 0 (FR-16.6 toasts). Overlays do not change the dispatcher's active scene.
- **Idle slack**: the residual Core 1 time inside the frame cap (`kFrameIntervalMs − render_time`). FR-16.4 / FR-16.5 work executes only when slack ≥ floor so frame rate is never sacrificed.

# Quantum Observatory — Coding Practices

Companion to [REQUIREMENTS.md](REQUIREMENTS.md) and [PLAN.md](PLAN.md). These rules apply to **every code change**. The `/continue-work` prompt references this file as a guardrail.

**Precedence:** REQUIREMENTS.md > this file > PLAN.md > existing code.

---

## 1. Memory Discipline (RP2040 has 264 KB)

- **No dynamic allocation in render or MQTT loops.** No `new`, `malloc`, `String` concat, `std::vector::push_back`, etc. after `setup()`. (NFR-2.2)
- **Scope of NFR-2.2 — engineering rule, not religion.** The risks the rule actually defends against (heap fragmentation, latency jitter, silent OOM, non-local failure) all come from **repeated, mixed-lifetime, hot-path** allocation. Apply it accordingly:
  - **Render loop (`loop1`, scene `render()`): hard ban.** No exceptions.
  - **MQTT callback / inbound message handling: hard ban.** Adversarial input can spam these.
  - **Other steady-state `loop()` work: hard ban by default.** Exception requires a one-line comment justifying why this allocation has bounded lifetime and bounded count.
  - **`setup()` / `begin()` / one-shot init: allowed if necessary** (it's just heap-resident-static with extra steps). Prefer static; if you allocate, document the size cap.
  - **Third-party library internals (lwIP, PubSubClient, WiFi, Protomatter): out of scope.** Audit at integration time, then trust. The libraries `malloc` somewhere and we can't avoid it without rewriting them.
  - When in doubt, prefer static — but don't contort the design to dodge a one-shot `new` in `setup()`. (added in phase 5.4)
- **Prefer `static` locals** for buffers and counters inside loop functions — zero-init guaranteed, no heap.
- **Fixed-size containers only** in hot paths: C arrays, `etl::array`, or hand-rolled ring buffers.
- **JSON:** always `StaticJsonDocument<N>`, never `DynamicJsonDocument`. Size = max documented payload + 25%. (NFR-2.3)
- **ArduinoJson v7 deprecation noise:** `StaticJsonDocument<N>` triggers `-Wdeprecated-declarations` because v7 wants you to use `JsonDocument` — but v7's `JsonDocument` allocates from the heap by default, violating NFR-2.2. Keep the deprecated class and scope-suppress the warning at each declaration site (`#pragma GCC diagnostic push/ignored "-Wdeprecated-declarations"/pop`) with a comment pointing at NFR-2.2. (added in phase 5.4)
- **Strings:** `const char*` and `char buf[N]` with `snprintf`. Never `String`.
- **Log free heap** in the status heartbeat (NFR-2.1). Track regressions.

## 2. Numeric Discipline

- **No `float` / `double` in render or hot loops.** RP2040 has no FPU — everything emulated. (NFR-1.3)
- Use **fixed-point Q8.8 / Q16.16** integer math via helpers (Phase 2.1).
- Trig and noise from **precomputed LUTs**, not `sinf()` / `cosf()`.
- **Q8.8 + 256-entry sin LUT live in `include/fixed_point.h`** — angle is `uint8_t` (auto-wraps at 256), amplitude is Q8.8 (±256). Call `fp::sin_cos_lut_init()` once in `setup()`. (added in phase 2.1)
- `float` is allowed only in `setup()`-time constant precomputation.
- Time math: always `uint32_t millis()` deltas. Handle 49-day wraparound by using `(now - then)` subtraction (which is wrap-safe), never `now > then`.
- **Animated scenes:** advance state by `dt = now_ms - last_ms` (clamp to ~100 ms to absorb stalls/init), keep position in Q8.8, and pre-multiply the per-layer step once per frame. Decouples motion from FPS. (added in phase 2.3)

## 3. Concurrency (Dual-Core)

- **Dual-core is a first-class design constraint, not an afterthought.** Every new feature, scene, sensor, or subsystem MUST answer "which core owns this, and why?" *before* code is written. Default placement (per §4.1 of REQUIREMENTS):
  - **Core 0 — Gatekeeper:** anything network-touching (Wi-Fi, MQTT, OTA), anything that parses adversarial input, sensor polling at ≤ 1 Hz, scene-lifecycle bookkeeping, watchdog feed, all `Serial` output.
  - **Core 1 — Artist & Compositor:** rendering, layer composition, per-frame animation state, speculative pre-render (FR-16.4), continuous sky-model (FR-16.5), and any computation whose result is consumed *only* by the next frame.
  - **Cross-core data:** read-mostly state uses the seqlock pattern (FR-16.7); edge events use `take_*()` IPC; multi-field state uses `mutex_t` with the shortest-window rule above.
- **Idle-slack is a budget, not free time.** Core 1 frame-caps at ~24 FPS (`kFrameIntervalMs` in `loop1()`). Before adding any heavy one-shot work to Core 0, ask whether Core 1's slack window (FR-16.9) is the better home — especially for anything time-correlated with rendering (palette rebuilds, asset prep, animation lookahead). Conversely: never push network or I²C work onto Core 1; it owns Protomatter PIO/DMA timing and any blocking call there is a flicker.
- **New code adds at minimum one log line per cross-core boundary it crosses** (writer side, on Core 0). Makes core-ownership bugs visible in the serial log instead of as mystery flicker.
- Shared state lives **only** in the `SceneState` struct. No other globals are read by both cores.
- All access to shared state goes through getter/setter helpers that take/release the mutex. **Never** read shared fields directly.
- Hold the mutex for **the shortest possible window** — copy out, release, then work on the copy.
- Cores must never `delay()` while holding a mutex.
- ISRs (if any) must not block. Push to a lock-free SPSC queue.
- **Multi-field cross-core state uses `mutex_t` (`pico/mutex.h`), not `volatile`.** The bool flag from 4.1 only works because it's a single one-shot signal; anything with ≥2 fields (e.g. `TimeOfDay`'s epoch + set_at + valid) needs a real mutex. Reader pattern: lock → copy fields into local snapshot → unlock → do math on the snapshot. Keeps the held window to a few word copies. (added in phase 3.5.1)
- **Cross-core "request once, consume once" IPC:** edge-triggered events (scene changes, mode-state updates) use a `take_*()` reader that atomically returns the value and clears the pending flag. Avoids re-init on every frame and keeps the writer side a single `request()` call. See `scene_state::take_pending()`. (added in phase 4.2)
- **Firmware-owned scene overrides** (sensor-driven safety/ambient modes — night, thermal_safe) are NOT requested by the Director. Pattern: a single `set_X_active(bool)` writer on Core 0 + a resolution rule inside `take_pending()` (`override_active ? OVERRIDE_SCENE : mqtt_requested`). The Director's last `request()` is preserved while the override holds, so falling-edge reverts naturally to the previously-requested scene with no extra bookkeeping. (added in phase 5.5.1)
- **Layered overrides resolve top-down** in a single `resolve()` helper: `thermal_active ? THERMAL_SAFE : night_active ? NIGHT : mqtt_requested`. Adding a fourth override = one new flag + one new line at the top of the chain. Crucial corollary: every `set_X_active()` setter MUST gate its `dirty=true` on `resolve(before) != resolve(after)`, NOT on the raw flag flip — otherwise toggling a *lower-priority* override while a higher one is masking it queues a spurious renderer wake. (added in phase 5.5.2)
- **Sensor poll modules expose `begin() + poll(now_ms) + decision accessor`.** `poll()` is internally rate-limited (its own min interval), returns `true` on a debounced state transition, and `false` otherwise. Caller hot-paths a single `if (poll()) publish()` line — no scheduling logic outside the module. See `light_sensor`. (added in phase 5.5.1)
- **Network-side modules are non-blocking state machines.** Wi-Fi/MQTT/OTA expose `begin()` (kicks off async work) + `poll(now_ms)` (drives transitions, called from `loop()`). Never block in `setup()` waiting for the radio — it stalls the watchdog (NFR-3.2) and the cross-core handshake. Backoff/retry is the module's job; callers just check `connected()`. See `wifi_link`. (added in phase 5.1)
- **MQTT subscriptions are re-issued on every (re)connect**, not once at `begin()`. PubSubClient sessions aren't persistent across our outages, so the broker forgets us when the TCP drops. `setCallback()` is set once in `begin()` (it's client-state, not session-state); each `subscribe()` lives in the `CONNECTING → CONNECTED` transition. (added in phase 5.3)
- **Inbound MQTT validation pattern (FR-1.3 / FR-1.4):** copy `payload[0..length)` into a `static`-sized stack buffer + `'\0'`, reject `length >= cap` early, parse with `StaticJsonDocument<N>`, log + drop on `DeserializationError` or missing required field. Never crash, never propagate malformed data past the callback. (added in phase 5.3)
- **Multi-topic MQTT dispatch:** the inbound callback does the buffer copy + size check ONCE up front, then dispatches by `strcmp(topic, kTopicX)` to per-topic handlers. Each handler owns its own `StaticJsonDocument<N>` (sized for that topic's payload), required-field check, and **range validation** before calling into the firmware. Use `doc[k].is<int>()` rather than `doc[k] | default` when "missing" must differ from a legal default value. (added in phase 5.5.3)
- **Priority preemption (FR-2.1) lives in `scene_state::request()`, not the MQTT handler.** Callers pass `(SceneId, priority)`; the writer drops the request iff `new_priority < current_priority` and returns `false` so the handler can log the reject. Equal priority is accepted (latest-wins, the §9 Open Question default). Firmware overrides (night/thermal_safe) bypass priority entirely (FR-7.5) — they never go through `request()`. The boot-time default scene MUST be requested at priority 0 so any Director request beats it. (added in phase 6.1)
- **Scene deadlines (FR-2.3 / FR-2.4) are 32-bit `millis()` stamps.** `request()` records `expires_at_ms` (soft, skipped when `sticky=true`) and `hard_ttl_at_ms` (always); a Core 0 `tick(now_ms)` reverts to the default `CLOCK @ prio 0` when either fires. Always compare with `static_cast<int32_t>(now - deadline) >= 0` to stay wrap-safe across the 49-day `millis()` rollover. The default itself is reverted-to but never reverts further — guard with an `already_default` short-circuit so the tick path is free once the panel is idle. (added in phase 6.2)

## 4. File & Symbol Layout

- **Pin/geometry config:** [include/config.h](../include/config.h) — single source of truth (NFR-5.2). No magic numbers in `.cpp` files.
- **I²C buses:** earlephilhower's `Wire1` accepts `setSDA()/setSCL()/setClock()` *before* `begin()`. Call all three with the `PIN_*_SDA/SCL` defines from `config.h`, then `Wire1.begin()` once in `setup()`. Do this on Core 0 — the matrix lives on Core 1 and the buses are independent. (added in phase 3.6.1)
- **Secrets:** `include/secrets.h` (gitignored). Provide `secrets.h.example`.
- **Pico W board id:** `board = rpipicow` in [platformio.ini](../platformio.ini). The plain `pico` board has no Wi-Fi stack — builds compile but radio calls are no-ops. (clarified in phase 5.1)
- **Scenes:** one file per scene under `src/scenes/`. Each implements the `Scene` interface — no other coupling.
- **Backgrounds:** ambient renderers live under `src/backgrounds/` as `*Bg` classes (no `Scene` interface, no `matrix.show()`). All bg state is held by a single `Backgrounds` instance (`g_backgrounds`) so multiple scenes share it. Composite scenes call `g_backgrounds.render(bg, ...)` first then draw foreground. (added in phase 2.5)
- **All color goes through the split palette.** Every renderer (background or foreground) reads from `palette::bg(Id, idx, shift)` (BG region 0..191, cyclic) or `palette::fg(Id, br6)` (FG region 192..255, linear brightness 0..63). Direct `matrix.color565(...)` calls in scene/bg code are forbidden — they bypass FR-12.1 and produce the same banding the LUT system was built to eliminate. Add a new palette Id rather than open-coding a one-off color. **Stops are perceptual sRGB-ish, NOT gamma-encoded** — `powf(x, 2.2)` collapses the dim end on this panel. (added in phase 3.7.2)
- **BMP assets are flash-resident, validated at build time.** Drop `assets/<name>.bmp` (8-bit indexed, uncompressed, 64×32, palette indices 0..191 only) and optionally `<name>.regions` for cycling sub-ranges. The `tools/bmp_to_header.py` pre-build hook generates `include/bitmaps/<name>.h` and re-emits `include/bitmaps/_index.h` (`kImageRegistry[]`). Build fails if any pixel index ≥ 192 — that's the contract that keeps FG palette entries reserved. Never hand-edit anything under `include/bitmaps/`. (added in phase 3.7.7)
- **Scenes never call `matrix.show()`.** The render pipeline in `loop1()` is: scene draws bg+fg → shared chrome (clock readout, future status icons) layered on top → `matrix.show()` exactly once. Scenes that want to suppress chrome (e.g. the giant clock) override `Scene::wants_clock_chrome()` to return `false`. (added in phase 3.5.2 to satisfy FR-9.3)
- **Text on animated bg:** always render via `gfx::draw_text_halo()` (FR-3.3). Plain `setCursor` + `print` is fine on solid black, but is illegal over any animated background. (added in phase 3.3)
- **2-line scenes:** use `gfx::draw_header(matrix, str)` and `gfx::draw_body(matrix, str)` rather than picking fonts and Y coords ad hoc. They own font choice, centring, baseline, and halo so layouts stay consistent across scenes (FR-4.3). (added in phase 3.4)
- **Halo cost is real:** measured ~238 FPS → ~69 FPS going from "nebula only" to "nebula + 2 haloed lines". Expected — halo rasterises each glyph 9×. Comfortably above the 30 FPS target but budget accordingly when adding more text. (measured in phase 3.3)
- **Adding a scene = registry entry + render function.** Nothing else changes. (NFR-5.1)
- **Wire-format `scene_id` strings live next to the `SceneId` enum** (`scene_state::id_from_string`), not in the MQTT layer. The MQTT callback only knows "string in → SceneId out → request()"; it never lists scenes. Keeps the §6 Scene Registry in one place. (added in phase 5.4)
- **Naming:**
  - Constants/macros: `UPPER_SNAKE`
  - Types/classes: `PascalCase`
  - Functions/methods: `lower_snake` (matches Pico SDK style)
  - File-static helpers: prefixed `s_` or `static`
  - Pin defines: `PIN_*`

## 5. Function & API Style

- **Small functions, single purpose.** If a function exceeds ~40 lines, split it.
- **Pass by `const&` or value**; return by value. No out-parameters except for performance-critical hot paths.
- **`constexpr` everything that can be.** Compile-time over runtime.
- **`static` (file-local linkage) for helpers.** Don't pollute the global namespace.
- **Mark functions `noexcept`** where applicable (we don't use exceptions, but explicit is better).
- No exceptions, no RTTI. Build flags should disable both.
- No `using namespace std;` at file scope.

## 6. Error Handling

- **Validate at boundaries** (MQTT input, JSON parse) — once accepted, trust the data internally.
- Graceful degradation > assertion failure. A bad message must **never** crash the device. (FR-1.4, NFR-3.3)
- Use return-code enums (`Status`) for fallible operations. No exceptions.
- Log errors with enough context to diagnose, but **don't spam** — rate-limit repeated errors.

## 7. Logging

- Format: `[<subsystem>] <message>` — e.g. `[mqtt] connected`, `[render] fps=302`.
- Subsystem tags lowercase: `boot`, `wifi`, `mqtt`, `render`, `scene`, `state`.
- **One serial line = one event.** Don't split a logical message across `Serial.print` + `println` calls if you can avoid it.
- Production logs are status info. Verbose tracing is `#ifdef DEBUG_<SUBSYS>`.
- Never log secrets, Wi-Fi passwords, or full MQTT credentials.

## 8. Comments

- Comment **why**, not **what**. The code already says what.
- **Always document non-obvious hardware quirks** (e.g. the FM6126A init sequence). Future-you will forget.
- **Always cite source** for ported code (URL, commit, file). The FM6126A init in main.cpp is the template.
- Avoid `TODO:` without a phase reference (e.g. `TODO(P5.3): wire to MQTT`).

## 9. Build & Verification

- `pio run --environment pico` must pass before any step is marked `[x]`. (PLAN.md gate)
- Watch the build output for **new warnings** — treat them as errors.
- After significant changes, eyeball the **RAM/Flash usage line**. Sustained growth needs investigation.
- Don't commit broken builds, even on a feature branch.

## 10. Hardware Safety

- **Never remove the FM6126A init sequence.** (FR-8) Without it the panel is dark.
- **Thermal safety is sensor-driven, not brightness-capped.** (FR-7.3, NFR-4.1) The firmware reads DS3231 on-die temperature and swaps to the `thermal_safe` scene above threshold — do not add a global brightness cap to short-circuit this; do avoid sustained full-white fills in scene authoring.
- **Protomatter must be constructed with double-buffer = true** (`PANEL_DOUBLE_BUFFER` in `config.h`). Single-buffer mode tears as horizontal white lines mid-refresh. (added in phase 2.2)
- **Protomatter is pinned to the core that calls `matrix.begin()`.** Its PIO state machines and DMA channels are claimed on that core; subsequent `matrix.show()` calls must come from the same core. Render-side init (FM6126A → `begin()` → scene `init()` → `render()`) all lives in `setup1()` / `loop1()`. (added in phase 4.1)
- **Cross-core handshake during boot:** Core 1 (`setup1()`) busy-waits on a `volatile bool` flag set at the end of `setup()` so it doesn't touch Serial or the fp LUT before Core 0 has initialised them. A plain `volatile bool` is sufficient because the write happens once and naturally-aligned bool stores are atomic on RP2040; promote to a mutex/FIFO only when more than one field crosses cores (Phase 4.2). (added in phase 4.1)
- **No `Serial.print` from Core 1 — ever.** USB-CDC writes block long enough to disrupt Protomatter's PIO/DMA refresh and produce visible per-row flicker. Core 1 publishes telemetry to a `volatile uint32_t` (e.g. `g_render_fps`); Core 0's `loop()` reads and prints it. The same rule applies to any other USB-CDC traffic on Core 1. (added in phase 3.7.1)
- **Bit depth caps fade resolution.** Per-channel weight math (AA, trail fade) only produces visible gradients if base color × weight stays ≥ 1 LSB at the panel's bit depth. We use `PANEL_BIT_DEPTH = 5` (32 levels, refresh ~190 Hz on this panel — 6 was too low and visibly flickered) and base colors ≥ ~25% so dim weights don't round to 0. The 5-bit limit is also why FR-12 routes all gradient work through palette LUTs instead of per-frame math. (revised in phase 3.7.1)
- **Hardware watchdog (NFR-3.2) gates on a Core 1 heartbeat.** RP2040 has a single 8 s WDT. Core 1 publishes `g_render_alive_ms = millis()` every `loop1()` iteration (before the frame cap can early-return); Core 0 only calls `rp2040.wdt_reset()` when the heartbeat is fresh (< half-timeout). Either core stalling → reset. Boot window is handled by treating `g_render_alive_ms == 0` as "always feed" so setup1's FM6126A init can't trip the WDT. Begin the WDT after the cross-core handshake but before any network init. (added in phase 6.5)
- Pin mode and direction are set explicitly in `setup()` — never assume reset defaults.
- Document any wiring assumptions in [include/config.h](../include/config.h) so they survive refactors.

## 11. Change Discipline

- **One step per session** (PLAN.md rhythm). Resist the urge to refactor things outside the current step.
- **No drive-by edits** — touch only files needed for the current step.
- **No new third-party deps without justification.** Each adds Flash and risk.
- **No new markdown docs** unless explicitly requested.

## 12. Git Hygiene

- Commit message format: `phase X.Y: <what works now>` (PLAN.md "Working Rhythm").
- One logical change per commit.
- `secrets.h`, `.pio/`, build artifacts: gitignored.

---

## When in doubt

- Check REQUIREMENTS.md first.
- Pick the **simpler** of two options.
- Ask the user before introducing complexity not justified by a requirement.

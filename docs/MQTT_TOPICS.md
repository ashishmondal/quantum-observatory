# Quantum Observatory — MQTT Topic Reference

Companion to [REQUIREMENTS.md](REQUIREMENTS.md). This is the **single source of truth** for the wire-level MQTT contract between Home Assistant (Director) and the Pico W (Cinematographer). Update this doc in the same commit as any change to topic name, payload shape, validation range, or subscription wiring in `src/mqtt_link.cpp`.

**Consistency rule:** every topic the firmware subscribes to in `mqtt_link.cpp` MUST appear here, and vice versa. Topic names, payload fields, and validation bounds in this doc MUST match the code exactly. The `/continue-work` prompt audits this on every run.

All payloads are JSON (UTF-8). Malformed JSON, missing required fields, and out-of-range values are logged and silently dropped per FR-1.3 / FR-1.4 — they never crash the device or interrupt the active scene.

---

## Subscriptions (HA → Pico)

### `observatory/scene` — trigger a scene

Payload (FR-1.1, REQUIREMENTS §5.1):

```json
{ "scene_id": "moon_phase", "priority": 2, "duration": 30, "sticky": true, "overrides": {} }
```

| Field | Type | Required | Default | Range | Notes |
|---|---|---|---|---|---|
| `scene_id` | string | yes | — | must be in [Scene Registry](REQUIREMENTS.md#6-initial-scene-registry-target-set-for-v10) | unknown id → drop (FR-1.3) |
| `priority` | int | no | 1 | 0..5 | clamped; lower than active scene → preempted (FR-2.1) |
| `duration` | int (sec) | no | 30 | 1..3600 | ignored when sticky (FR-2.3) |
| `sticky` | bool | no | false | — | persists until clear/override/TTL (FR-2.2) |
| `overrides` | object | no | `{}` | — | scene-specific (`text`, `val`, …) — currently logged only |

Example:
```bash
mosquitto_pub -t observatory/scene -m '{"scene_id":"moon_phase","priority":2,"sticky":true}'
```

### `observatory/clear_sticky` — release a sticky scene

Payload: empty (any payload tolerated). FR-2.2 / REQUIREMENTS §5.3.

```bash
mosquitto_pub -t observatory/clear_sticky -m ''
```

### `observatory/night` — LDR threshold tuning (FR-7.4)

```json
{ "threshold": 1500, "hysteresis": 100 }
```

| Field | Type | Range | Notes |
|---|---|---|---|
| `threshold` | int (12-bit ADC) | 0..4095 | trip point for night-mode swap |
| `hysteresis` | int (12-bit ADC) | 1..4095 | must be > 0 (zero would chatter) |

### `observatory/thermal` — DS3231 thermal threshold tuning (FR-7.4)

```json
{ "threshold": 60, "hysteresis": 5 }
```

| Field | Type | Range | Notes |
|---|---|---|---|
| `threshold` | int (°C) | -40..125 | swap to `thermal_safe` above this |
| `hysteresis` | int (°C) | 1..50 | must be > 0 |

### `observatory/time` — RTC correction (FR-9.5)

```json
{ "epoch_utc": 1714608000, "tz_offset_min": -300 }
```

| Field | Type | Range | Notes |
|---|---|---|---|
| `epoch_utc` | long (Unix sec) | 1577836800..4102444800 (2020-01-01..2100-01-01) | DS3231 BCD year limit |
| `tz_offset_min` | int (min) | -720..840 | IANA range with slack |

This is the *correction* path only — readers always go through `tod::now()` against the RTC. (FR-9.5)

### `observatory/moon` — lunar state push for `moon_phase` scene

```json
{ "phase": 0.34, "illum_pct": 68, "age_d": 10, "name": "WAX GIB" }
```

| Field | Type | Required | Range | Notes |
|---|---|---|---|---|
| `phase` | float | yes | 0.0..<1.0 | synodic-month fraction (0=new, 0.5=full) |
| `illum_pct` | int | yes | 0..100 | percent illumination |
| `age_d` | int | yes | 0..30 | days since last new moon |
| `name` | string | no | ≤11 chars | scene derives 8-bucket name if omitted |

Snapshot is treated as fresh for **12 h** (`moon_state::kFreshMs`). After that, the scene falls back to local synodic-month math from the RTC. (Phase 7.2 follow-up)

```bash
mosquitto_pub -t observatory/moon -m '{"phase":0.5,"illum_pct":100,"age_d":15,"name":"FULL"}'
```

### `observatory/iss` — raw HA pass-through for the `iss_pass` scene

```json
{ "lat_deg": 50.11, "lon_deg": 118.07, "altitude_km": 408, "sunlit": true, "seconds_until_next": 12345, "crew_count": 7 }
```

| Field | Type | Required | Range | Notes |
|---|---|---|---|---|
| `lat_deg` | float (°) | yes | -90..90 | ISS sub-satellite latitude (+N / -S) |
| `lon_deg` | float (°) | yes | -180..180 | ISS sub-satellite longitude (+E / -W) |
| `altitude_km` | int (km) | yes | 0..999 | orbital altitude (rounded to nearest km) |
| `sunlit` | bool | yes | — | true = ISS is in sunlight (NOT observer-visibility — see "On-device derivation" below) |
| `seconds_until_next` | long (sec) | yes | 0..604800 (≤ 1 week) | seconds until next visible pass *starts*; the scene projects the live tick-down via a millis() delta against the push timestamp |
| `crew_count` | int | no | 0..99 | current crew aboard — scene renders `CREW 7`, or `CREW ?` when absent/stale. Optional because crew changes weekly and HA may not have polled `astros.json` yet on cold boot. |

If any **required** field is missing, malformed, or out of range, the
whole payload is dropped per FR-1.3 / FR-1.4 — the scene keeps using
the previous fresh snapshot (or "WAIT" if none). An out-of-range
`crew_count` is the only field that's tolerated as a partial update:
it gets demoted to "absent" while the rest of the payload still
applies.

**Where HA gets the data — and why HA does no logic.** The intent is
that HA polls public REST APIs and re-emits the fields verbatim
through a Jinja template. No template arithmetic, no observer-frame
geometry, no "is the station above MY horizon" decisions — all of
that is computed on-device every frame in the `iss_pass` scene.

- `lat_deg` / `lon_deg` / `altitude_km` / `sunlit` —
  `https://api.wheretheiss.at/v1/satellites/25544` →
  `.latitude`, `.longitude`, `round(.altitude)`,
  `.visibility != "eclipsed"`. WTIA's `visibility` field is
  tri-state: `"daylight"` (sub-satellite point in daylight, ISS
  sunlit), `"visible"` (sub-satellite point at night but ISS still
  catches the sun — i.e. the dusk/dawn case where you can actually
  see it), `"eclipsed"` (ISS in Earth's shadow, invisible from
  anywhere). Mapping `sunlit = (visibility != "eclipsed")` is the
  only correct collapse to a boolean. Default units are kilometers.
  **Rate limit ≈ 350 req / 5 min** per the WTIA docs (watch the
  `X-Rate-Limit-*` response headers); the publisher polls every 30 s
  for ~97% headroom.
- `seconds_until_next` — computed in pyscript by propagating the
  ISS orbit forward 7 days with skyfield + a CelesTrak TLE
  (`https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE`,
  cached for 6 h). For each rise event from
  `EarthSatellite.find_events(observer, t0, t1, altitude_degrees=10°)`
  we filter for `is_sunlit(eph)` AND sun ≤ −6° at the observer at
  the rise instant — i.e. the same three-way AND the firmware
  evaluates per frame, applied to the future. The first rise that
  passes is the next *visible* pass; its unix time minus `now()`
  is the published value. Capped at 604800 (= 7 d, the doc max);
  if no visible pass falls in the window (e.g. high summer at low
  latitudes when twilight never goes deep enough) we publish that
  cap, which the firmware renders as `VIS IN 7D` rather than lying.
  *Was previously sourced from `http://api.open-notify.org/iss-pass.json`
  which has been HTTP 404 since ~2021.*
- `crew_count` — `http://api.open-notify.org/astros.json` →
  `.people | selectattr('craft','eq','ISS') | count`. Note the
  filter is necessary — the top-level `.number` field also includes
  Tiangong crew. Polled hourly by an HA REST sensor.

**On-device derivation (no firmware TLE math, no HA template
math).** Every render frame the scene computes:

1. **Look angles** from `(LATITUDE_DEG, LONGITUDE_DEG)` and
   `(lat_deg, lon_deg, altitude_km)` via closed-form
   spherical-Earth geometry (`iss_geom::look_angles()` —
   ~150 µs). Yields `bearing_deg` (compass) + `elevation_deg`
   (above horizon).
2. **Sun elevation** at the observer via `sun::compute()` against
   the live RTC UTC epoch — already used by the sky background.
3. **Visibility** as the three-way AND:
    - ISS is sunlit (`sunlit == true`), AND
    - observer is in twilight or darker (sun elevation ≤ −6°), AND
    - ISS is above the observer horizon (`elevation_deg ≥ 0`).
   When all three hold, the line renders `VIS BBBxEE` (e.g.
   `VIS 180x45`); otherwise the countdown branch runs.

The WTIA `visibility` field's name is famously misleading — it
describes the *satellite's* lighting state at its sub-satellite
point, not whether you can see it from your backyard. Hence why HA
forwards it as the literal `sunlit` boolean and the firmware
combines it with the other two conditions itself.

When **not** visible the countdown collapses to a single unit:

| Time to next pass | Rendered as |
|---|---|
| ≥ 1 day | `VIS IN 3D` (rounded to nearest day) |
| ≥ 1 hour, < 1 day | `VIS IN 5H` (rounded to nearest hour) |
| < 1 hour | `VIS IN 12M` (floored, clamped ≥ 1) |
| ≤ 0 (HA late) | `VIS SOON` |

Snapshot is treated as fresh for **1 h** (`iss_state::kFreshMs`). After
that, the scene falls back to a `WAIT` placeholder rather than
fabricating a stale countdown. Push at any cadence ≤ 1 h that suits
the Director — the firmware smooths between pushes second by second.

```bash
mosquitto_pub -t observatory/iss -m '{"lat_deg":50.11,"lon_deg":118.07,"altitude_km":408,"sunlit":true,"seconds_until_next":12345,"crew_count":7}'
mosquitto_pub -t observatory/iss -m '{"lat_deg":-12.4,"lon_deg":42.7,"altitude_km":410,"sunlit":false,"seconds_until_next":7200}'
```

### `observatory/jupiter` — raw HA pass-through for the `jupiter_visibility` scene

```json
{ "bearing_deg": 90, "elevation_deg": 45, "magnitude": -2.1, "distance_au": 5.4, "constellation_index": 76 }
```

| Field | Type | Required | Range | Notes |
|---|---|---|---|---|
| `bearing_deg` | float (°) | yes | 0..360 | Compass azimuth (0=N, 90=E). Wrapped + rounded to int [0,359] on-device. |
| `elevation_deg` | float (°) | yes | -90..90 | Altitude above horizon. Negative = below horizon → `BELOW`. |
| `magnitude` | float | no | -30..30 | Apparent magnitude (Jupiter ≈ -2.9 to -1.6 in practice). Stored ×10 fixed-point on-device for float-free render (NFR-1.3). Renders `MAG -2.1`, or `MAG ?` when absent. |
| `distance_au` | float (AU) | no | 0..100 | Earth–Jupiter distance (≈ 4..6 AU in practice). Renders `DIST 5.4AU`, or `DIST ?` when absent. |
| `constellation_index` | int | yes | 0..87 (= `constellations_iau::kCatalogCount-1`) | Index into the same IAU catalog used by `observatory/constellation` (sorted `And`, `Ant`, ... `Vol`). When Jupiter is above the horizon but the observer isn't dark enough yet, the line-3 readout is `IN <IAU>` (e.g. `IN TAU`). Missing or out-of-range rejects the whole payload. |

If any **required** field is missing, malformed, or out of range,
the whole payload is dropped per FR-1.3 / FR-1.4. Out-of-range
optional fields are demoted to "absent" without rejecting the rest
(same partial-update pattern as `observatory/iss`'s `crew_count`).

**Where HA gets the data — and why HA does no logic.** Same
pass-through contract as `observatory/iss`: HA polls any astronomy
integration that exposes Jupiter's apparent position (e.g.
ephemeris/astroweather components built on pyephem/skyfield) and
re-emits the four fields verbatim through a Jinja template. No
template arithmetic, no observer-frame visibility decisions — the
ephemeris already gives the look-angles in the observer's frame, so
the firmware only needs to decide whether the *observer* is in
darkness.

**On-device derivation.** Every render frame the
`jupiter_visibility` scene computes:

1. **Sun elevation** at the observer via `sun::compute()` against
   the live RTC UTC epoch — same call the iss_pass scene uses.
2. **Visibility** as the two-way AND:
    - Jupiter is above the observer horizon (`elevation_deg ≥ 0`), AND
    - observer is in twilight or darker (sun elevation ≤ −6°).

Unlike the ISS payload there's no `sunlit` field — Jupiter is *always*
sunlit (planets shine by reflected light), so only the observer's
darkness condition matters for naked-eye visibility.

The line-3 readout is one of:

| Condition | Rendered as |
|---|---|
| Above horizon AND sun ≤ −6° | `VIS BBBxEE` (e.g. `VIS 090x45`) |
| Below horizon | `BELOW` |
| Above horizon, sun > −6° | `IN <IAU>` (e.g. `IN TAU`) |
| No fresh data | `WAIT` |

Snapshot is treated as fresh for **1 h** (`jupiter_state::kFreshMs`).
Jupiter's apparent position drifts ~0.5 °/h max so an hour-stale
snapshot still points the kid at roughly the right patch of sky;
past that the scene falls back to `WAIT` rather than fabricating a
stale pointing string. Push at any cadence ≤ 1 h that suits the
Director.

```bash
mosquitto_pub -t observatory/jupiter -m '{"bearing_deg":90,"elevation_deg":45,"magnitude":-2.1,"distance_au":5.4,"constellation_index":76}'
mosquitto_pub -t observatory/jupiter -m '{"bearing_deg":270,"elevation_deg":-12,"constellation_index":58}'
```

### `observatory/constellation` — Director-pushed selector for the `constellation_now` scene

```json
{ "index": 0, "highlight_star": 1 }
```

| Field | Type | Required | Range | Notes |
|---|---|---|---|---|
| `index` | int | yes | 0..87 (= `constellations_iau::kCatalogCount-1`) | Index into the firmware-side IAU constellation catalog (`include/stars.h`, generated by `tools/skyculture_to_header.py` from the Stellarium "western" sky culture). Sorted by IAU 3-letter code (`And`, `Ant`, `Aps`, ... `Vol`). Out-of-range renders the scene's local rotation fallback rather than a `WAIT` placeholder. |
| `highlight_star` | int | no | 0..63, or -1 | **Brightness rank** (NOT HIP) within the chosen constellation: 0 = brightest visible star, 1 = second brightest, etc. The picked star pulses red with a + cross; if it has a proper name (e.g. Betelgeuse) that name replaces the Latin name on typewriter line 2. -1 (or omit) clears the highlight. Out-of-range for the chosen entry's star count is harmless (the scene checks). |

If `index` is missing/malformed, the whole payload is dropped per
FR-1.3 / FR-1.4. Out-of-range `highlight_star` is demoted to "no
highlight" without rejecting the rest (same partial-update pattern
as `observatory/iss`'s `crew_count`).

**Where HA gets the data — and why this is the lightest contract.**
Unlike the iss/jupiter pass-throughs, there's no public REST API
that gives "the constellation overhead from MY backyard right now."
The Director-side decision is a small lookup against the firmware
catalog's well-known order. HA can pick by:

- Date-driven seasonal rotation: a Jinja template indexes by month
  (Dec/Jan/Feb → Orion, Mar/Apr/May → Leo, etc).
- Simple time-of-night offset: walk the index every 30 min during
  active hours.
- Static featured-of-the-week selector chosen by the family.

The firmware catalog covers all 88 IAU constellations (regenerated
from Stellarium data, no per-entry hand-authoring). HA discovers
what's available by the index range; no separate manifest needed.
The scene's local rotation fallback (every 30 s through the catalog)
means it works standalone with HA down — useful during initial
bring-up.

**On-device rendering.** The chosen entry's stars are projected from
real (RA, Dec) into the right-half 30×26 sub-region every frame:

- Per-constellation bounding-box projection with `cos(dec_center)`
  RA scaling and isotropic aspect (the smaller axis scale wins) so
  shapes don't distort.
- East-on-left + North-on-top, matching naked-eye view facing south.
- Stars sized + brightened by V magnitude (`mag_x100`): mag<1 gets a
  full + cross with twinkle, mag<2 gets a cross without tips, mag<3
  a single bright pixel, fainter stars dim further.
- Asterism lines connect the catalog's `LineSegment` pairs in dim
  warm grey.
- Highlighted star (when `highlight_star` is set) overrides the
  normal magnitude render with a pulsing red + cross, and its proper
  name (when known) takes over typewriter line 2.

See [REQUIREMENTS.md](REQUIREMENTS.md) §6 for the scene registry
entry and `include/stars.h` for the catalog layout. Regenerate
`stars.h` by running `python tools/skyculture_to_header.py` after
changing `assets/sky-culture.json` or the Hipparcos source CSV
(see [../assets/README.md](../assets/README.md)).

Snapshot is treated as fresh for **24 h** (`constellation_state::kFreshMs`).
The "constellation overhead right now" changes ~once an hour, so
even a half-day-stale push is still pointing at a nearby
constellation. Past that the scene's local 30 s rotation takes over.

```bash
mosquitto_pub -t observatory/constellation -m '{"index":0}'
mosquitto_pub -t observatory/constellation -m '{"index":0,"highlight_star":1}'
mosquitto_pub -t observatory/constellation -m '{"index":2,"highlight_star":-1}'
```

### `observatory/theme` — active retro sci-fi theme (FR-15.2)

```json
{ "id": "apollo_amber" }
```

| Field | Type | Required | Range | Notes |
|---|---|---|---|---|
| `id` | string | yes | one of `apollo_amber`, `nostromo_green`, `vectrex_neon`, `blade_runner`, `lcars_tos` | lowercase wire id matching `theme::Id` enumerators (FR-15.1). Unknown ids → drop (FR-1.3). |

The swap takes effect at the next frame boundary with no scene re-init
(FR-15.4). `theme::set()` is idempotent — re-publishing the active id
is a no-op. The active theme is **not** persisted across reboots
(FR-15.2 — no flash wear); the firmware boots to `apollo_amber` and
HA is expected to push the desired theme on every reconnect.

The active theme is echoed back in `observatory/status.theme`
(FR-15.7) so the Director can confirm without round-tripping this
topic.

```bash
mosquitto_pub -t observatory/theme -m '{"id":"apollo_amber"}'
mosquitto_pub -t observatory/theme -m '{"id":"nostromo_green"}'
```

---

## Publications (Pico → HA)

### `observatory/status` — heartbeat (REQUIREMENTS §5.4)

Published every 30 s.

```json
{ "scene_id": "clock", "fps": 24, "rssi": -55, "uptime_s": 1234, "free_heap": 180000, "render_slack_ms": 21, "theme": "apollo_amber" }
```

| Field | Type | Notes |
|---|---|---|
| `scene_id` | string | active scene wire-id (matches `observatory/scene` payloads) |
| `fps` | int | Core 1 rendered frames in the last 1 s window |
| `rssi` | int (dBm) | Wi-Fi RSSI at heartbeat time |
| `uptime_s` | int (sec) | `millis() / 1000` since boot |
| `free_heap` | int (bytes) | `rp2040.getFreeHeap()` — track regressions per NFR-2.1 |
| `render_slack_ms` | int (ms) | Rolling 32-frame average of `kFrameIntervalMs - render_time` on Core 1 (FR-16.9). High = idle headroom; falling toward 0 = scene is using the full frame budget. |
| `theme` | string | active retro sci-fi theme wire-id (FR-15.7); matches `observatory/theme` payloads. |

### `observatory/debug` — one-shot diagnostic dumps (phase IR.2)

Published opportunistically by debug/diagnostic scenes when they have
something to report — NOT a steady-state heartbeat. Each message is a
self-describing JSON document with an `event` discriminator the
listener can switch on; new event types may be added without bumping a
version. Currently emitted by:

- `IrTestScene` (the IR-learning wizard) on completion — one
  `event: "ir_learn"` message containing the captured NEC address +
  per-button command codes for the operator to lift into
  `include/config.h` (FR-17.3).

```json
{
  "event": "ir_learn",
  "address": 85,
  "button_count": 9,
  "captures": [
    { "name": "home",    "proto": 8, "addr": 85, "cmd": 10, "raw": 4244766975 },
    { "name": "up",      "proto": 8, "addr": 85, "cmd":  6, "raw": 4244504831 },
    { "name": "down",    "proto": 8, "addr": 85, "cmd":  7, "raw": 4244570367 }
  ]
}
```

| Field | Type | Notes |
|---|---|---|
| `event` | string | discriminator — currently only `"ir_learn"` |
| `address` | int | NEC address byte common to the captured remote (HOME's address; FR-17.3 expected-address gate uses this) |
| `button_count` | int | number of capture slots in the table |
| `captures[].name` | string | logical button name from the FR-17.5 mapping (`home`, `up`, `down`, `left`, `right`, `ok`, `back`, `options`, `replay`) |
| `captures[].proto` | int | `decode_type_t` value (8 = NEC) |
| `captures[].addr` | int | NEC address byte (will normally match top-level `address`) |
| `captures[].cmd` | int | NEC command byte — the value to bake into `kIrButton<Name>Cmd` |
| `captures[].raw` | long | low 32 bits of the protocol-specific payload (for debugging only) |

QoS 0 / not retained — diagnostic dumps shouldn't persist on the broker.

Subscribe with:
```bash
mosquitto_sub -t observatory/debug -v
```
then trigger `{"scene_id":"ir_test"}` and walk through every prompt on
the panel.

### `observatory/test/clock_anim` — synthetic clock-cascade trigger (dev-only)

Subscribed **only** when the firmware is built with `-DCLOCK_ANIM_TEST`
(off by default). Lets the developer fire a giant-clock digit-roll
cascade (FR-9.7) on demand without waiting for the wall clock.

```json
{ "kind": "minute" }
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `kind` | string | yes | One of `"minute"` (M2 only), `"ten_min"` (M1 cascade), `"hour"` (H2 cascade). Unknown values are logged and dropped (FR-1.3). |

Production builds silently ignore the topic (the subscription is
compiled out). Not meant to be wired into HA automations — exists
purely for bench-side animation tuning.

---

## Reconnect Behaviour

- All subscriptions above are **re-issued on every (re)connect** — non-persistent broker sessions don't survive our outages (CODING_PRACTICES §3).
- Wi-Fi/MQTT outage triggers exponential backoff 1s → 60s (FR-5.2).
- After ~5 s of disconnect the panel switches to the `offline` scene (FR-5.1).

---

## Adding a New Topic — Checklist

1. Add `kTopicX = "observatory/<name>"` constant in `src/mqtt_link.cpp`.
2. Implement `handle_x(buf, length, now_ms)` mirroring the validation pattern from existing handlers (size cap → JSON parse → required-field check → range check → log + drop on any failure).
3. Dispatch to it in `on_mqtt_message()` via `strcmp`.
4. Add `kTopicX` to the topics array re-subscribed inside the `CONNECTING → CONNECTED` transition.
5. **Add a row in the relevant section of this document.**
6. If the topic feeds a scene, mention it in the scene's docblock.

`/continue-work` will flag any drift between the constants in `mqtt_link.cpp` and this file.

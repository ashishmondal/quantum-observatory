# Future Scenes — Backlog

Long-tail scene ideas for the Quantum Observatory dashboard, organised
by feasibility tier. Audience target: high-school teen, **80% "what's
in the sky tonight" / 20% "how the universe works"** (per
[REQUIREMENTS.md](REQUIREMENTS.md) audience note).

This is a **brainstorming backlog** — not a commitment, not ordered by
priority within a tier. Items get promoted into [PLAN.md](PLAN.md)
phase entries when they're picked up. Each entry sketches the wire
contract + on-device derivation in enough detail that a future
"continue-work" session can pick it up cold.

The architectural rule from FR-14 applies to every entry: **HA does
no observer-frame logic**, only raw API pass-through. The firmware
does the visibility AND, the look-angle math, the freshness fallback.

---

## Tier 1 — Natural Fits

Same architectural pattern as `iss_pass` / `moon_phase` /
`jupiter_visibility`: HA pushes raw fields, firmware does derivation,
1 h freshness window, opt-in clock chrome where the BMP corner allows.

### `saturn_visibility`
Clone of `jupiter_visibility` with `saturn.bmp`. Wire fields:
`bearing_deg`, `elevation_deg`, optional `magnitude`, `distance_au`,
optional `ring_tilt_deg`. Hook: rings tilt cycles ~15 yr — render
"RING TILT 18°" when HA can supply.

### `mars_visibility`
Same template again. Bonus line: distance in **light-minutes**
("MARS 12LM") — kid-friendly framing of light-travel time.

### `venus_phase`
Venus has Galilean phases like the moon (Galileo's original
geocentrism-killer observation). Reuses the moon shader from
`moon_phase` for the phase overlay on `venus.bmp`. Lines:
`PHASE` / `ILL %d%%` / `VIS DDDxEE`. Educational hook: planets
aren't point sources, they have geometry.

### `mercury_visibility`
Hardest naked-eye planet to catch — only visible briefly at
twilight. Visibility logic should highlight the narrow windows:
`VIS NOW` only fires when sun is between -3° and -10°.

### `iss_passes_today`
Sticky variant of `iss_pass`. Three lines = next 3 pass start
times tonight. `RISE 21:14` / `RISE 22:46` / `RISE 00:18`. HA pulls
from open-notify or sat-tracker.

### `tonight_planets`
Single-screen overview: which naked-eye planets are above horizon
*right now*, sorted by brightness. 3 lines = top 3 with format
`JUP W 45° -2.1`. One scene, full sky summary. **Likely the highest
"what can I see now" payoff per scene.**

### `star_of_the_night`
Sticky featured star, rotating nightly: Sirius / Vega / Betelgeuse /
Polaris / Arcturus / etc. Lines: name / constellation / direction +
altitude. HA picks one different one each night.

### `sunrise_sunset`
Pure RTC + `sun::compute()` — **zero MQTT**. 3 lines:
`SUNRISE 06:14` / `SUNSET 19:42` / `DAY 13H28M`. Bonus stat:
solar-noon altitude. Free win, dashboard stays alive when HA is down.

### `twilight_now`
Live state machine: `DAY → CIVIL → NAUTICAL → ASTRO → NIGHT`, with
a coloured bar showing current sun altitude. Teaches the −6/−12/−18°
bands the firmware is already using internally for ISS visibility.
Pure on-device computation — no MQTT.

### `moonrise_moonset`
Companion to `moon_phase`. Lines: `RISE 14:32` / `SET 03:18` /
`TRANSIT 21:55`. Pure ephemeris from HA.

---

## Tier 2 — Astrophysics Flavor (the 20%)

The pieces that turn the dashboard from an ephemeris into a small
science classroom.

### `light_travel_time`
Daily rotation of "how long ago did this light leave?" Cycles a
different object each minute: `PROXIMA / 4Y 3M`,
`ANDROMEDA / 2.5MY`, `M87 / 53MY`, `CMB / 13.8GY`. Builds intuition
for cosmic distance.

### `spectrum_demo`
A 64-pixel-wide spectrum bar (palette LUT system can render this
beautifully, 380→700 nm gradient) with a moving marker labelled by
the element causing that absorption line: `HYDROGEN-α 656nm`,
`SODIUM-D 589nm`, etc. Rotates through a few. Astronomy 101 visual.

### `hr_diagram`
Static `hr.bmp` of the Hertzsprung–Russell scatter, with a single
overlay pixel highlighting the night's "featured star" position.
Ambitious in 64×32 but doable.

### `exoplanet_count`
`TODAY: 5847 EXOPLANETS` / `+12 THIS WEEK` / `NEAREST 4LY`. Pulled
from NASA Exoplanet Archive via HA daily. The "number is going up"
loop has built-in engagement.

### `orbital_mechanics`
Animated 1-pixel "Earth" orbiting the sun with current Earth angle
correct for date. Teaches axial-tilt → seasons. Same engine could
demonstrate Kepler's 2nd law (pixel speeds up at perihelion).

### `black_hole_news`
Sticky: most recent published black-hole / gravitational-wave
event. Lines: `GW250414` / `2 BH MERGE` / `1.2BLY`. HA polls
LIGO/Virgo public alerts (GraceDB has a JSON feed).

### `solar_activity`
Current sunspot count + solar wind speed + Kp index (geomagnetic
storm). Hook: `AURORA TONIGHT?` verdict line. NOAA SWPC has free
JSON feeds. Genuinely useful for a stargazing kid.

### `lagrange_points`
Educational rotation: shows L1/L2/L3/L4/L5 of the Earth-Sun system
with a tiny diagram + which spacecraft live there (`L2: JWST`,
`L1: SOHO`). One per minute.

---

## Tier 3 — High-Engagement / High-Spectacle

The scenes that get the kid to *show their friends*.

### `meteor_shower`
Sticky during shower windows (Perseids / Geminids / Quadrantids /
etc.). Lines: `PERSEIDS` / `60/HR` / `RAD NE`. Bonus: animated
1-pixel streaks across the bg. HA has a static calendar; firmware
reads RTC date.

### `launch_countdown`
Next rocket launch from a public API (Rocket Launch Live / The
Space Devs). `LAUNCH` / `SX FALCON9` / `IN 4H 12M`. High novelty
for a teenager.

### `satellite_pass` (generalised)
Same engine as `iss_pass`, swappable target: Hubble / Tiangong /
Starlink train / Tianhe. The "Starlink train just after sunset"
moment is a gateway-drug astronomy experience.

### `andromeda_pointer`
Sticky novelty: `ANDROMEDA` / `2.5MY` / `NE 35°`. The fact that
you can see a whole galaxy with the naked eye is mind-blowing if
you've never been told. Replace with M31 / M42 / M13 / M81 on
nightly rotation.

### `what_am_i_looking_at`
Input direction (configured "I'm pointing at the W window" — or a
real magnetometer if hardware grows), output the brightest object in
that direction right now. Static-window version is one config line.

### `day_in_history`
"ON THIS DATE: 1969 APOLLO 11" cycling daily. Pure offline lookup
table baked into firmware (~365 entries × 32 chars ≈ 12 KB —
within budget). High educational density per byte.

---

## Tier 4 — Physics Interactive / Nerd-Bait

Conversation-starter scenes. Lower priority but very low cost.

### `relativity_ticker`
`TIME DILATION AT ISS / 0.007s/YR`. Cycles through "time dilation
at this orbit / mass / velocity" examples — surface, ISS, GPS sat,
Mercury orbit, neutron star surface.

### `scale_of_universe`
Zooming counter: `ATOM 10⁻¹⁰m` → `CELL 10⁻⁵m` → … →
`OBSERVABLE 10²⁶m`, one decade per 2 sec, with a representative
bmp per decade. Powers-of-ten in 64×32.

### `fermi_paradox`
Rotating one-line provocations: `DRAKE EQUATION: 12 CIVS`,
`GREAT FILTER?`, `WHERE ARE THEY?`. Pure conversation starter.

### `cosmic_calendar`
Sagan's "compress 13.8 Gyr into one year" calendar, shown for
**today's date in cosmic time**. May 4 ≈ ~10:30 PM December 31 in
cosmic time → "HUMANS APPEAR 10S AGO". Daily refresh from RTC.

---

## Tier 5 — Non-Celestial

Resist scope creep here (weather is explicitly out per REQUIREMENTS),
but a couple of personal-status-board ideas earn their slot.

### `now_observing`
Manual override topic — HA can push "currently photographing M42 —
240/300 subs" if the kid ever picks up astrophotography. Personal
status board.

### `astronomy_word_of_day`
Vocab builder. `APHELION` / `PARALLAX` / `LAGRANGE` / `SYZYGY`.
One word + one-line definition. Teen-friendly 30s read. Pure offline
table.

---

## Sequencing Suggestions

If picking 2–3 phases after 7.5 (HA wiring), in priority order:

1. **`tonight_planets`** — biggest "what can I actually see right
   now" payoff for one scene. Reuses `jupiter_visibility` shape
   almost verbatim; satisfies the 80% astronomy bias.
2. **`sunrise_sunset` + `twilight_now`** — zero MQTT cost, pure RTC.
   Free wins that keep the dashboard alive when HA is down.
3. **`meteor_shower`** + **`launch_countdown`** — sticky scenes,
   high novelty, kid will tell their friends.
4. **`exoplanet_count`** + **`solar_activity`** — the astrophysics
   20%. Both have real data feeds, both genuinely change over time
   so the dashboard rewards being looked at.

`constellation_now` is already promoted to PLAN.md phase 7.4.

---

## Promotion Checklist

When promoting a scene from this backlog into PLAN.md:

1. Add a row to the §6 Scene Registry in [REQUIREMENTS.md](REQUIREMENTS.md).
2. Add a `[ ] **7.x** scene_id — short description` line in PLAN.md.
3. If the scene needs a new MQTT topic, also add a §-section in
   [MQTT_TOPICS.md](MQTT_TOPICS.md) in the same commit (per the
   /continue-work Step 3.7 rule).
4. If the scene needs new BMP art, add `assets/<name>.bmp` and the
   pre_build hook will regenerate `include/bitmaps/_index.h`.
5. Remove (or strike through) the entry from this file once
   delivered.

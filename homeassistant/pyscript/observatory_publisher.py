# Quantum Observatory — Pyscript ephemeris publisher
# ===================================================
#
# Replaces three of the YAML "stub" automations in
# packages/quantum_observatory.yaml with real astronomy:
#
#   observatory/jupiter        every 15 min
#   observatory/moon           every  6 h
#   observatory/constellation  every  1 h
#
# All three are computed with `skyfield` (MIT, bundled DE421
# ephemeris, sub-arcsecond accuracy). The firmware contracts
# in include/{jupiter,moon,constellation}_state.h are unchanged
# — pyscript publishes the same JSON shape the YAML stubs did,
# just with real numbers.
#
# Why this lives in pyscript (not a YAML template):
# --------------------------------------------------
# Home Assistant ships no planetary ephemeris. The choices were:
#   * AstroWeather (HACS) — a heavyweight integration with its
#     own opinions about UI; pulls 30+ entities we don't need.
#   * pyscript + skyfield — ~150 lines of Python, no extra UI,
#     uses the existing MQTT integration. Picked for footprint.
#
# Skyfield + pyscript pitfall, with the fix
# -----------------------------------------
# Skyfield does file I/O on first ephemeris load (~17 MB DE421
# .bsp file). Pyscript's reference docs explicitly warn that
# blocking I/O in the main HA event loop produces "blocking
# call detected" warnings. The fix is to wrap every skyfield
# call in `@pyscript_executor` so it runs on a worker thread.
# Every compute function below uses that decorator.
#
# Setup checklist (handled by ../setup_mqtt.py --install-publisher)
# ----------------------------------------------------------------
#   1. Pyscript HACS integration installed.
#   2. configuration.yaml has:
#         pyscript:
#           allow_all_imports: true   # needed to import skyfield
#           hass_is_global: true      # needed to read hass.config
#   3. /config/pyscript/requirements.txt contains `skyfield`
#      (pyscript auto-pip-installs on next reload).
#   4. This file is at /config/pyscript/observatory_publisher.py
#   5. Pyscript reload triggered (or HA restarted).
#
# All publishes use retain=False — same reason as the YAML
# automations: the firmware caches every inbound payload for
# 1–24 h per topic's kFreshMs, and a retained message would
# only leak stale data after an HA outage.

import json

# Skyfield's loader caches downloaded ephemerides on disk. We
# pin it under /config/ so the cache survives container restarts
# (the default ~/.skyfield would be /root/.skyfield in HA OS,
# which a Supervisor reset would wipe — re-downloading 17 MB
# from JPL on every reset is needlessly hostile to the network).
SKYFIELD_CACHE = "/config/skyfield_data"

# 88 IAU constellation 3-letter codes in the EXACT order they
# appear in the firmware's `constellations_iau::kCatalog[]` (see
# include/stars.h). The C++ array was produced by
# `tools/skyculture_to_header.py` with `out.sort(key=lambda c: c.iau)`,
# i.e. straight Python `sorted()` over title-case strings — which
# puts uppercase ASCII before lowercase, so "CMa" < "Cae".
#
# The integer published on observatory/constellation is the index
# into this list (which equals the C++ array index by construction).
# DO NOT alphabetise this list "properly" — it must match the
# firmware's sort order byte-for-byte.
IAU_CODES = [
    "And", "Ant", "Aps", "Aql", "Aqr", "Ara", "Ari", "Aur", "Boo",
    "CMa", "CMi", "CVn", "Cae", "Cam", "Cap", "Car", "Cas",
    "Cen", "Cep", "Cet", "Cha", "Cir", "Cnc", "Col", "Com",
    "CrA", "CrB", "Crt", "Cru", "Crv", "Cyg", "Del", "Dor",
    "Dra", "Equ", "Eri", "For", "Gem", "Gru", "Her", "Hor",
    "Hya", "Hyi", "Ind", "LMi", "Lac", "Leo", "Lep", "Lib",
    "Lup", "Lyn", "Lyr", "Men", "Mic", "Mon", "Mus", "Nor",
    "Oct", "Oph", "Ori", "Pav", "Peg", "Per", "Phe", "Pic",
    "PsA", "Psc", "Pup", "Pyx", "Ret", "Scl", "Sco", "Sct",
    "Ser", "Sex", "Sge", "Sgr", "Tau", "Tel", "Tra", "Tri",
    "Tuc", "UMa", "UMi", "Vel", "Vir", "Vol", "Vul",
]
_IAU_INDEX = {code: i for i, code in enumerate(IAU_CODES)}


# ---------------------------------------------------------------------
# Skyfield-backed compute functions — ALL wrapped in @pyscript_executor
# so the disk I/O for the ephemeris .bsp happens on a worker thread,
# never the HA main event loop. Each call re-imports skyfield and
# re-loads the ephemeris; skyfield caches the parsed file in memory
# inside the loader so the second call onward is fast (~1 ms).
# ---------------------------------------------------------------------

@pyscript_executor
def _compute_jupiter(lat_deg, lon_deg):
    """Compute Jupiter's apparent position + magnitude for an observer.

    Returns a dict matching the observatory/jupiter wire schema
    (docs/MQTT_TOPICS.md), or None on any skyfield error.
    """
    try:
        from skyfield.api import wgs84, Loader, load_constellation_map
        from skyfield.magnitudelib import planetary_magnitude

        loader = Loader(SKYFIELD_CACHE)
        eph = loader('de421.bsp')
        ts = loader.timescale()
        t = ts.now()

        earth = eph['earth']
        jupiter = eph['jupiter barycenter']
        observer = earth + wgs84.latlon(lat_deg, lon_deg)
        apparent = observer.at(t).observe(jupiter).apparent()
        alt, az, dist = apparent.altaz()
        mag = float(planetary_magnitude(apparent))

        # Host constellation — the IAU patch Jupiter currently sits
        # in, used by the firmware's daylight readout ("IN TAU").
        # Required on the wire (firmware rejects the payload without
        # it), so an unknown skyfield code aborts the publish rather
        # than emitting a partial payload.
        constellation_at = load_constellation_map()
        code = constellation_at(apparent)
        if code in ("Ser1", "Ser2"):
            code = "Ser"
        con_idx = _IAU_INDEX.get(code)
        if con_idx is None:
            return {"_error": f"unknown constellation code from skyfield: {code!r}"}

        # Clamp to wire ranges (docs/MQTT_TOPICS.md). The firmware
        # also range-checks but a clean publish keeps logs readable.
        return {
            "bearing_deg":         int(round(az.degrees)) % 360,
            "elevation_deg":       max(-90, min(90, int(round(alt.degrees)))),
            "magnitude":           round(max(-30.0, min(30.0, mag)), 1),
            "distance_au":         round(max(0.0, min(100.0, dist.au)), 2),
            "constellation_index": con_idx,
        }
    except Exception as exc:  # noqa: BLE001 — log, don't crash the trigger
        return {"_error": f"{type(exc).__name__}: {exc}"}


@pyscript_executor
def _compute_moon():
    """Compute synodic-month phase + illumination for the current UTC.

    Uses the sun-earth-moon ecliptic-longitude difference (Meeus §49).
    No observer location needed — moon phase is geocentric and the
    firmware's display contract (`name`) maps to 8 buckets that don't
    care about parallax.
    """
    try:
        import math
        from skyfield.api import Loader

        loader = Loader(SKYFIELD_CACHE)
        eph = loader('de421.bsp')
        ts = loader.timescale()
        t = ts.now()

        sun, earth, moon = eph['sun'], eph['earth'], eph['moon']
        e = earth.at(t)
        _, sun_lon, _ = e.observe(sun).apparent().ecliptic_latlon()
        _, moon_lon, _ = e.observe(moon).apparent().ecliptic_latlon()

        # Synodic phase 0..1: 0=new, 0.25=first qtr, 0.5=full, 0.75=last qtr
        phase_deg = (moon_lon.degrees - sun_lon.degrees) % 360.0
        phase_frac = phase_deg / 360.0

        # Illumination fraction (Meeus eq. 48.1, simplified for phase angle):
        # k = (1 - cos(phase_deg)) / 2 when phase_deg measured from sun-moon
        illum_pct = int(round((1.0 - math.cos(math.radians(phase_deg))) * 50.0))
        illum_pct = max(0, min(100, illum_pct))

        # Age in days since last new moon. Synodic month = 29.530588 d.
        age_d = int(round(phase_frac * 29.530588))

        # 8-bucket display name matching the firmware's existing
        # render. Bucket boundaries are slightly asymmetric to match
        # what the YAML stub (mapped from HA's `sensor.moon`) produced,
        # so a/b testing during the rollout shows the same names on
        # the same dates. Keep names ≤ 11 ASCII chars (display line).
        if illum_pct < 3:
            name = "NEW"
        elif phase_frac < 0.22:
            name = "WAX CRES"
        elif phase_frac < 0.28:
            name = "FIRST QTR"
        elif phase_frac < 0.47:
            name = "WAX GIB"
        elif phase_frac < 0.53:
            name = "FULL"
        elif phase_frac < 0.72:
            name = "WAN GIB"
        elif phase_frac < 0.78:
            name = "LAST QTR"
        elif phase_frac < 0.97:
            name = "WAN CRES"
        else:
            name = "NEW"

        return {
            "phase":     round(phase_frac, 3),
            "illum_pct": illum_pct,
            "age_d":     age_d,
            "name":      name,
        }
    except Exception as exc:  # noqa: BLE001
        return {"_error": f"{type(exc).__name__}: {exc}"}


@pyscript_executor
def _compute_constellation(lat_deg, lon_deg):
    """Pick which IAU constellation is closest to the observer's zenith.

    Returns {"index": 0..87, "highlight_star": 0} where `index` is the
    position in the firmware's `constellations_iau::kCatalog[]` (see
    IAU_CODES at the top of this file). The brightest star in that
    constellation is highlighted by convention (highlight_star=0 is the
    rank-0 / brightest).

    "What's overhead" is: take the observer's local zenith RA/Dec at
    the current UTC, then ask skyfield's IAU 1930 constellation
    boundary table which constellation owns that point. That's the
    constellation a kid pointing straight up at the dashboard would
    actually see directly above their house — which is the upgrade
    over the YAML stub's month-of-year lookup table.
    """
    try:
        from skyfield.api import Loader, load_constellation_map, position_of_radec

        loader = Loader(SKYFIELD_CACHE)
        # Constellation lookup doesn't actually need the planetary
        # ephemeris, but we load it anyway so a cold start populates
        # the cache for the next jupiter/moon trigger.
        loader('de421.bsp')
        ts = loader.timescale()
        t = ts.now()

        # Zenith on the celestial sphere has RA = local sidereal time
        # and Dec = observer latitude. This is straight spherical
        # astronomy — no ephemeris needed, no observer-position object,
        # just the rotation of the Earth. Skyfield gives us GST in
        # hours; LST = GST + (longitude_east / 15°/h).
        gst_hours = t.gmst
        lst_hours = (gst_hours + lon_deg / 15.0) % 24.0

        zenith = position_of_radec(lst_hours, lat_deg, t=t)
        constellation_at = load_constellation_map()
        code = constellation_at(zenith)             # e.g. "Ori", "Ser1", "Ser2"

        # Skyfield reports the two halves of Serpens as "Ser1" / "Ser2";
        # the firmware catalog has one "Ser" entry covering both.
        if code in ("Ser1", "Ser2"):
            code = "Ser"

        idx = _IAU_INDEX.get(code)
        if idx is None:
            return {"_error": f"unknown constellation code from skyfield: {code!r}"}

        return {"index": idx, "highlight_star": 0}
    except Exception as exc:  # noqa: BLE001
        return {"_error": f"{type(exc).__name__}: {exc}"}


# ---------------------------------------------------------------------
# ISS — TLE fetch + next-visible-pass prediction (replaces the dead
# open-notify /iss-pass.json endpoint, which has been HTTP 404 since
# ~2021). We pull the current TLE for catalog 25544 from CelesTrak
# (the canonical free TLE re-host for the ISS) and run skyfield's
# SGP4 propagator to find the next time the station rises ≥ 10° above
# the observer's horizon AND is itself sunlit AND the observer is in
# civil twilight or darker — i.e. when the on-device three-way AND
# would actually flip true.
#
# Two module-level caches keep the cost down:
#   * _TLE_CACHE      — refreshed every 6 h. CelesTrak refreshes
#                       several times daily; 6 h is the cadence
#                       they recommend for non-rendezvous use.
#   * _NEXT_PASS_CACHE — refreshed every 30 min by
#                       refresh_iss_next_pass(). The 30 s publisher
#                       reads it without re-running find_events.
#                       Find_events over a 7-day window is the
#                       expensive part (~2-3 s); we don't need it
#                       on every publish since pass times only shift
#                       seconds across an hour.
# ---------------------------------------------------------------------

# CelesTrak's "GP" (general perturbations) endpoint. CATNR=25544 is
# the ISS (ZARYA module — the original 1998 launch). Asking for plain
# TLE format gives us two ~70-char lines we feed straight into
# skyfield's EarthSatellite constructor.
_ISS_TLE_URL = "https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE"
_TLE_TTL_SEC = 6 * 3600     # 6 h
_NEXT_PASS_TTL_SEC = 30 * 60  # 30 min

# Mutable module state — pyscript runs this file in a single
# interpreter, so a top-level dict is the right shape for "value
# computed by trigger A, read by trigger B". Both refresh_* and
# publish_iss touch only their own keys, no locks needed.
_TLE_CACHE: dict = {"line1": None, "line2": None, "fetched_at": 0.0}
_NEXT_PASS_CACHE: dict = {
    "lat": None, "lon": None,
    "unix": None,           # unix timestamp of next visible rise, or None
    "computed_at": 0.0,     # monotonic-ish; we use time.time()
}


@pyscript_executor
def _fetch_iss_tle():
    """Fetch the current ISS TLE from CelesTrak. Returns (line1, line2)
    or None on any error. Network I/O lives behind @pyscript_executor
    so the HA event loop never blocks on it.

    CelesTrak occasionally returns a "No GP data found" plain-text
    body when their backend is briefly stale — we treat that as a
    transient error and reuse the previous cached TLE.
    """
    try:
        import urllib.request
        req = urllib.request.Request(
            _ISS_TLE_URL,
            headers={"User-Agent": "quantum-observatory/1.0 (+https://github.com)"},
        )
        with urllib.request.urlopen(req, timeout=15) as resp:
            body = resp.read().decode("ascii", errors="replace").strip()
        lines = [ln.strip() for ln in body.splitlines() if ln.strip()]
        # Expect 3 lines: name, line1, line2. Be defensive — some
        # CelesTrak responses omit the name when the format query
        # is set, others include it. Match the two real TLE lines
        # by their leading "1 " / "2 " markers.
        l1 = next((ln for ln in lines if ln.startswith("1 25544")), None)
        l2 = next((ln for ln in lines if ln.startswith("2 25544")), None)
        if not l1 or not l2:
            return {"_error": f"CelesTrak response missing TLE lines: {body[:120]!r}"}
        return {"line1": l1, "line2": l2}
    except Exception as exc:  # noqa: BLE001
        return {"_error": f"{type(exc).__name__}: {exc}"}


@pyscript_executor
def _compute_iss_next_pass(lat_deg, lon_deg, tle_line1, tle_line2):
    """Find the next time (unix UTC) the ISS will be visible from the
    observer in the next 7 days. Returns {"unix": int} or
    {"unix": None} if no visible pass falls in the window (e.g. high
    summer at low latitudes when twilight never goes deep enough).

    "Visible" matches the firmware's three-way AND from
    src/scenes/iss_pass_scene.h:
      A. ISS sunlit at the rise time (sat.at(t).is_sunlit(eph))
      B. Sun ≤ -6° at observer at the rise time (civil twilight)
      C. Implicit — find_events with altitude_degrees=10° guarantees
         the station rises high enough for a comfortable pass.

    altitude_degrees=10 (vs 0) filters out grazing horizon-skimmers
    that most pass-prediction sites also exclude — they never get
    bright enough to notice over local light pollution.
    """
    try:
        from skyfield.api import wgs84, Loader, EarthSatellite

        loader = Loader(SKYFIELD_CACHE)
        eph = loader('de421.bsp')
        ts = loader.timescale()

        sat = EarthSatellite(tle_line1, tle_line2, "ISS (ZARYA)", ts)
        observer = wgs84.latlon(lat_deg, lon_deg)
        sun = eph['sun']
        earth = eph['earth']

        from datetime import timedelta
        t0 = ts.now()
        t1 = ts.from_datetime(t0.utc_datetime() + timedelta(days=7))

        # find_events returns (Time array, event-code array) where
        # event codes are 0=rise, 1=culminate, 2=set. We only care
        # about rises here — each rise is a candidate pass start.
        times, events = sat.find_events(observer, t0, t1, altitude_degrees=10.0)

        observer_topos = earth + wgs84.latlon(lat_deg, lon_deg)

        for ti, ev in zip(times, events):
            if ev != 0:
                continue
            # Gate A: is the station itself sunlit at this moment?
            if not bool(sat.at(ti).is_sunlit(eph)):
                continue
            # Gate B: sun ≤ -6° (civil twilight or darker) at observer?
            sun_alt, _, _ = observer_topos.at(ti).observe(sun).apparent().altaz()
            if sun_alt.degrees > -6.0:
                continue
            # Both pass — this is the next *visible* pass start.
            return {"unix": int(ti.utc_datetime().timestamp())}

        # No visible pass in 7 days — honest answer is "none".
        return {"unix": None}
    except Exception as exc:  # noqa: BLE001
        return {"_error": f"{type(exc).__name__}: {exc}"}


def _compose_iss(lat_deg, lon_deg):
    """Read live state from the WTIA + astros REST sensors (defined
    in ../packages/quantum_observatory.yaml), combine with the
    cached next-pass time, and return the observatory/iss payload
    dict matching docs/MQTT_TOPICS.md.

    Returns {"_error": ...} if WTIA hasn't cached a position yet
    (cold boot, first 30 s) — _publish() then logs and skips, the
    firmware shows WAIT, which is exactly the documented behaviour.
    """
    try:
        import time

        # WTIA: position + altitude + visibility tri-state. The HA
        # REST sensor caches every 30 s; we read its attributes.
        # state.getattr returns a dict, or None if the entity is
        # unknown/unavailable.
        attrs = state.getattr("sensor.iss_position")
        if not attrs:
            return {"_error": "sensor.iss_position not yet available"}
        try:
            lat = float(attrs["latitude"])
            lon = float(attrs["longitude"])
            alt = int(round(float(attrs["altitude"])))
        except (KeyError, TypeError, ValueError) as exc:
            return {"_error": f"WTIA attrs malformed: {exc}"}

        # Three-state visibility: "daylight" | "visible" | "eclipsed".
        # ISS is sunlit in the first two — only "eclipsed" means it's
        # in Earth's shadow and physically invisible from anywhere.
        # The previous YAML mapping (visibility == "daylight") was
        # backwards: it returned False for "visible", which is the
        # exact dusk/dawn case the firmware's three-way AND is
        # designed to catch.
        vis_str = attrs.get("visibility", "eclipsed")
        sunlit = (vis_str != "eclipsed")

        # Cached next-pass unix time (refreshed every 30 min by
        # refresh_iss_next_pass). On cold boot it can be None for up
        # to one refresh cycle — treat that as "no countdown known"
        # and clamp seconds_until_next to the doc's max (604800 = 7d),
        # which the firmware renders as "VIS IN 7D".
        next_unix = _NEXT_PASS_CACHE.get("unix")
        if next_unix is None:
            sec_to_next = 604800
        else:
            sec_to_next = max(0, int(next_unix - time.time()))
            sec_to_next = min(sec_to_next, 604800)

        payload = {
            "lat_deg": round(lat, 4),
            "lon_deg": round(lon, 4),
            "altitude_km": max(0, min(999, alt)),
            "sunlit": sunlit,
            "seconds_until_next": sec_to_next,
        }

        # Optional crew_count from the astros REST sensor. Absent
        # rather than guessed if the sensor hasn't polled yet — the
        # firmware renders "CREW ?" in that case (partial-update
        # rule per docs/MQTT_TOPICS.md).
        crew_raw = state.get("sensor.iss_crew_count")
        if crew_raw not in (None, "unknown", "unavailable"):
            try:
                crew = int(crew_raw)
                if 0 <= crew <= 99:
                    payload["crew_count"] = crew
            except (TypeError, ValueError):
                pass

        return payload
    except Exception as exc:  # noqa: BLE001
        return {"_error": f"{type(exc).__name__}: {exc}"}


# ---------------------------------------------------------------------
# Time-driven publish triggers. Pyscript runs each as its own task,
# so the three are independent — a slow skyfield call on one topic
# never delays the others.
# ---------------------------------------------------------------------
#
# `hass_is_global: true` (in pyscript yaml config) exposes the `hass`
# variable; we read observer location from `zone.home` to match what
# the YAML automations did, with a hardware-config fallback so a
# fresh HA install with no zones still publishes something. The
# firmware's include/config.h LATITUDE_DEG / LONGITUDE_DEG should
# match these — keep them in sync if you move.

def _observer_lat_lon():
    """Read observer lat/lon from HA. Returns (lat, lon) floats."""
    try:
        lat = float(hass.config.latitude)
        lon = float(hass.config.longitude)
        return lat, lon
    except Exception:  # noqa: BLE001
        # Hard fallback — these match the firmware default in config.h.
        # Override by setting Settings → System → General → Location.
        return 0.0, 0.0


def _publish(topic, payload_dict):
    """Wrapper around mqtt.publish that logs the outcome and writes a
    success-witness HA state.

    The state entity (`pyscript.observatory_publisher_<slug>`) carries
    the ISO-8601 UTC timestamp of the last successful publish, with the
    payload stashed in attributes for debugging. setup_mqtt.py's
    --verify-publisher reads this entity's `last_changed` to prove the
    publish actually happened end-to-end. State is more reliable than
    log-grepping `/api/error_log` (that endpoint returns 404 in some
    HA configurations) and survives log rotation.
    """
    # Slug: "observatory/jupiter" → "observatory_publisher_jupiter".
    # Keeps every entity under one obvious prefix in Developer Tools →
    # States so they're easy to find.
    slug = "observatory_publisher_" + topic.split("/", 1)[1]
    entity_id = f"pyscript.{slug}"

    if "_error" in payload_dict:
        log.error(f"observatory_publisher: {topic} skipped: {payload_dict['_error']}")
        # Record the failure on the witness entity too — a stale
        # last_changed + an _error attribute tells the operator the
        # publisher fired but the compute failed (vs. didn't fire at all).
        state.set(entity_id, "error", new_attributes={
            "topic": topic,
            "error": payload_dict["_error"],
        })
        return

    payload = json.dumps(payload_dict, separators=(",", ":"))
    # qos=1 (at-least-once): qos:0 silently drops on any Wi-Fi blip
    # between HA → broker → firmware, leaving the firmware stuck on
    # the previous payload for up to `kFreshMs` of that topic (e.g.
    # 24 h for jupiter/moon/constellation). One PUBACK round-trip per
    # ≤hourly publish is a trivial cost for guaranteed delivery.
    # Matches the qos:1 setting on every YAML mqtt.publish in
    # ../packages/quantum_observatory.yaml.
    service.call(
        "mqtt", "publish",
        topic=topic, payload=payload, retain=False, qos=1,
    )

    # ISO-8601 UTC, second precision — matches HA's own datetime
    # rendering and parses cleanly with datetime.fromisoformat() on
    # both ends. We don't rely on this string for the freshness check
    # (setup_mqtt reads `last_changed` from the state metadata), but
    # it's handy in the UI.
    from datetime import datetime, timezone
    iso_now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S+00:00")
    state.set(entity_id, iso_now, new_attributes={
        "topic":   topic,
        "payload": payload,
    })
    log.info(f"observatory_publisher: {topic} → {payload}")


# Run all three at startup so the dashboard fills in within seconds
# of HA boot, then on each topic's natural cadence.

@service
@time_trigger("startup", "cron(*/15 * * * *)")
def publish_jupiter(**_):
    """Jupiter — every 15 min. Firmware kFreshMs = 1 h, so we have
    4× headroom for missed publishes.

    Also exposed as service `pyscript.publish_jupiter` so
    homeassistant/setup_mqtt.py --verify-publisher can force-call it."""
    lat, lon = _observer_lat_lon()
    _publish("observatory/jupiter", _compute_jupiter(lat, lon))


@service
@time_trigger("startup", "cron(0 */6 * * *)")
def publish_moon(**_):
    """Moon — every 6 h on the hour. Firmware kFreshMs = 12 h.

    Also exposed as service `pyscript.publish_moon`."""
    _publish("observatory/moon", _compute_moon())


@service
@time_trigger("startup", "cron(0 * * * *)")
def publish_constellation(**_):
    """Constellation — top of every hour. Firmware kFreshMs = 24 h.
    The constellation overhead drifts ~15°/h so an hourly refresh
    gives the observatory a constellation that's actually overhead
    most of the time, not just "what month is it".

    Also exposed as service `pyscript.publish_constellation`."""
    lat, lon = _observer_lat_lon()
    _publish("observatory/constellation", _compute_constellation(lat, lon))


@service
@time_trigger("startup", "cron(*/30 * * * *)")
def refresh_iss_next_pass(**_):
    """Refresh the cached next-visible-pass time every 30 min.

    Two-stage refresh:
      1. If the in-memory TLE is older than 6 h (or absent), fetch a
         fresh one from CelesTrak. On network failure the previous
         TLE keeps being used — SGP4 accuracy degrades slowly (the
         common rule of thumb is ~1 km/day position error for ISS),
         so a TLE up to a day stale still gives pass times accurate
         to the second.
      2. Run skyfield's find_events over the next 7 days and stash
         the unix timestamp of the next visible rise in
         _NEXT_PASS_CACHE. publish_iss reads this on every 30 s tick.

    Exposed as service `pyscript.refresh_iss_next_pass` so
    setup_mqtt.py --verify-publisher can force-call it.
    """
    import time

    now = time.time()
    lat, lon = _observer_lat_lon()

    # Stage 1: TLE refresh if stale.
    age = now - _TLE_CACHE.get("fetched_at", 0.0)
    if not _TLE_CACHE.get("line1") or age > _TLE_TTL_SEC:
        result = _fetch_iss_tle()
        if "_error" in result:
            log.warning(
                f"observatory_publisher: TLE refresh failed: {result['_error']}"
                + (" (using stale TLE)" if _TLE_CACHE.get("line1") else " (no TLE yet)")
            )
            if not _TLE_CACHE.get("line1"):
                # Nothing to compute against — bail until next cycle.
                return
        else:
            _TLE_CACHE["line1"] = result["line1"]
            _TLE_CACHE["line2"] = result["line2"]
            _TLE_CACHE["fetched_at"] = now
            log.info("observatory_publisher: ISS TLE refreshed from CelesTrak")

    # Stage 2: find next visible pass.
    pass_result = _compute_iss_next_pass(
        lat, lon, _TLE_CACHE["line1"], _TLE_CACHE["line2"]
    )
    if "_error" in pass_result:
        log.warning(
            f"observatory_publisher: next-pass compute failed: {pass_result['_error']}"
        )
        return

    _NEXT_PASS_CACHE["lat"] = lat
    _NEXT_PASS_CACHE["lon"] = lon
    _NEXT_PASS_CACHE["unix"] = pass_result["unix"]
    _NEXT_PASS_CACHE["computed_at"] = now

    # Stash on a witness entity for operator visibility (Developer
    # Tools → States → pyscript.iss_next_pass_unix shows when the
    # next visible pass is, in human-readable ISO).
    if pass_result["unix"] is not None:
        from datetime import datetime, timezone
        iso = datetime.fromtimestamp(
            pass_result["unix"], tz=timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%S+00:00")
        state.set(
            "pyscript.iss_next_pass_unix",
            str(pass_result["unix"]),
            new_attributes={"iso_utc": iso, "computed_at": now},
        )
        log.info(f"observatory_publisher: next ISS visible pass at {iso}")
    else:
        state.set(
            "pyscript.iss_next_pass_unix",
            "none",
            new_attributes={"note": "no visible pass in 7-day window",
                            "computed_at": now},
        )
        log.info("observatory_publisher: no visible ISS pass in next 7 days")


@service
@time_trigger("startup", "period(now, 30sec)")
def publish_iss(**_):
    """Publish observatory/iss every 30 s.

    Combines:
      - WTIA position + sunlit (from sensor.iss_position, polled by
        the rest: block in ../packages/quantum_observatory.yaml)
      - Cached next-visible-pass time (from refresh_iss_next_pass)
      - Crew count (from sensor.iss_crew_count, polled hourly)

    Replaces the YAML `observatory_iss` automation that used to live
    in quantum_observatory.yaml. Two reasons for the move:
      1. The sunlit mapping needed to be (visibility != "eclipsed"),
         not (visibility == "daylight") — the latter silently dropped
         visibility=="visible", which is exactly the dusk/dawn case
         that makes the station observable in the first place.
      2. open-notify's /iss-pass.json (the YAML's source for
         seconds_until_next) returns HTTP 404 — has been dead since
         ~2021. We now compute next-pass on-host with skyfield + a
         CelesTrak TLE.

    Also exposed as service `pyscript.publish_iss`.
    """
    lat, lon = _observer_lat_lon()
    _publish("observatory/iss", _compose_iss(lat, lon))

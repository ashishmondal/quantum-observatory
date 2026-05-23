# Quantum Observatory — Pyscript ephemeris publisher
# ===================================================
#
# Replaces three of the YAML "stub" automations in
# packages/quantum_observatory.yaml with real astronomy:
#
#   observatory/planet         every 15 min
#   observatory/moon           every  6 h
#   observatory/constellation  every  1 h
#
# All three are computed with `skyfield` (MIT, bundled DE421
# ephemeris, sub-arcsecond accuracy). The firmware contracts
# in include/{planet,moon,constellation}_state.h are unchanged
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
# All publishes use retain=False by default — same reason as the
# YAML automations: the firmware caches every inbound payload for
# 1–24 h per topic's kFreshMs, and a retained message would
# only leak stale data after an HA outage.
#
# Exception: observatory/launch is published retained. Cron fires
# only once an hour (cron(7 * * * *)) and the firmware's freshness
# clock is keyed on RECEIVE time, not the t0 epoch, so a device
# that reboots or reconnects mid-hour would otherwise sit on
# "AWAITING SCHEDULE" for up to ~60 min until the next tick.
# Retention is harmless here because the firmware re-validates t0
# against tod::now() at apply time — a stale retained launch whose
# t0 has slipped past `now - 3600 s` is rejected (handle_launch
# bounds check), not rendered.
_RETAIN_TOPICS = {"observatory/launch"}

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

# Skyfield body key per supported `name`. Earth is intentionally
# absent — you can't be on Earth and observe Earth. The Moon has
# its own dedicated observatory/moon topic and isn't routed here.
# Anything outside this map returns _error and the publish is
# dropped.
_SKYFIELD_BODY_BY_NAME = {
    "sun":     "sun",
    "mercury": "mercury",
    "venus":   "venus",
    "mars":    "mars",
    "jupiter": "jupiter barycenter",
    "saturn":  "saturn barycenter",
    "uranus":  "uranus barycenter",
    "neptune": "neptune barycenter",
}

@pyscript_executor
def _compute_planet(lat_deg, lon_deg, name):
    """Compute a named body's apparent position for an observer.

    Returns a dict matching the observatory/planet wire schema
    (docs/MQTT_TOPICS.md), or None / {'_error': ...} on any
    skyfield error. `name` must be one of the keys in
    _SKYFIELD_BODY_BY_NAME; everything else short-circuits.

    Magnitude is no longer computed — the firmware scene's line-3
    overlay only needs look-angles + host constellation. The wire
    format dropped `magnitude` / `distance_au` when the scene was
    generalised; those values are still pulled here only for the
    log line so a human eyeballing the trigger output can sanity-
    check the ephemeris.
    """
    try:
        if name not in _SKYFIELD_BODY_BY_NAME:
            return {"_error": f"unsupported body name: {name!r}"}
        body_key = _SKYFIELD_BODY_BY_NAME[name]

        from skyfield.api import wgs84, Loader, load_constellation_map

        loader = Loader(SKYFIELD_CACHE)
        eph = loader('de421.bsp')
        ts = loader.timescale()
        t = ts.now()

        earth = eph['earth']
        body  = eph[body_key]
        observer = earth + wgs84.latlon(lat_deg, lon_deg)
        apparent = observer.at(t).observe(body).apparent()
        alt, az, _dist = apparent.altaz()

        # Host constellation — the IAU patch the body currently sits
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

        return {
            "name":                name,
            "bearing_deg":         int(round(az.degrees)) % 360,
            "elevation_deg":       max(-90, min(90, int(round(alt.degrees)))),
            "constellation_index": con_idx,
        }
    except Exception as exc:  # noqa: BLE001 — log, don't crash the trigger
        return {"_error": f"{type(exc).__name__}: {exc}"}


# ---------------------------------------------------------------------
# NASA Exoplanet Archive — phase 7.7 exoplanet_count scene.
#
# Pulled via the public TAP synchronous endpoint
# (https://exoplanetarchive.ipac.caltech.edu/TAP/sync). Two queries:
#   1. total count of confirmed exoplanets
#   2. nearest known planet by sy_dist (distance in parsecs)
#
# Network-only — no skyfield, no ephemeris. Lives behind
# @pyscript_executor so the urllib requests don't block HA's event
# loop. Failure modes (network error, archive HTTP 5xx, schema
# change) return {"_error": ...} which _publish() then drops without
# touching the wire (firmware sees no update and falls back to its
# 48 h freshness window or finally to "WAIT").
# ---------------------------------------------------------------------

# Parsecs → light-years conversion factor (CODATA-derived, exact to
# six digits — the archive's sy_dist precision is well below this).
_PC_TO_LY = 3.26156

# TAP base; we add per-query overrides at call time.
_EXOARCHIVE_TAP = (
    "https://exoplanetarchive.ipac.caltech.edu/TAP/sync"
)

# Polite User-Agent so the archive admins can identify traffic on the
# off chance our daily poll ever shows up in their logs as suspicious.
# NASA docs don't publish a hard rate limit for the sync TAP endpoint,
# but daily-cadence polling with a clear identifier is the right thing.
_EXOARCHIVE_UA = "quantum-observatory/1.0 (+https://github.com/) pyscript"


@pyscript_executor
def _compute_exoplanet():
    """Compute current exoplanet count + nearest-known planet.

    Returns a dict matching the observatory/exoplanet wire schema
    minus `added_recent` (added by the @service caller after rolling-
    window math), or {"_error": ...} on any failure.

    Validated 2026-05-22: TAP endpoint returns JSON arrays of dicts;
    `select top N ... order by` is BROKEN on pscomppars (Oracle
    applies the rownum cap before the sort), so the nearest query
    has to wrap the sort with a `min()` sub-select. Don't "simplify"
    it back to top-N-order-by without re-verifying against live
    data — the previous form silently returned Kepler-1581 b (493 pc)
    instead of Proxima Cen b (1.30 pc) and looked superficially fine.
    """
    try:
        import urllib.parse
        import urllib.request
        import json as _json

        def _tap_get(query):
            url = (_EXOARCHIVE_TAP + "?"
                   + urllib.parse.urlencode(
                        {"query": query, "format": "json"}))
            req = urllib.request.Request(
                url, headers={"User-Agent": _EXOARCHIVE_UA})
            with urllib.request.urlopen(req, timeout=15) as r:
                return _json.loads(r.read().decode("utf-8"))

        # --- 1. total confirmed-exoplanet count ----------------------
        # pscomppars = planetary systems composite parameters, the
        # archive's "one row per planet, best-available values" table.
        # count(*) on pscomppars matches the headline "confirmed
        # exoplanets" number on the archive's home page.
        rows = _tap_get("select count(*) as n from pscomppars")
        if not rows or "n" not in rows[0]:
            return {"_error": "exoplanet total: empty or malformed response"}
        total_count = int(rows[0]["n"])

        # --- 1b. year-to-date discoveries ----------------------------
        # Used by _exoplanet_added_recent to bootstrap a synthetic
        # back-dated history entry on first run — lets the firmware
        # show a real YTD-pace-extrapolated weekly delta from day 1
        # instead of "+0 WK" until measured history accumulates.
        # disc_year is `int`, no quoting needed. NASA TAP supports
        # extract(year from ...) but disc_year is the canonical field
        # for this and avoids a function call on every row.
        from datetime import datetime as _dt, timezone as _tz
        current_year = _dt.now(_tz.utc).year
        rows = _tap_get(
            f"select count(*) as n from pscomppars "
            f"where disc_year = {current_year}"
        )
        ytd_count = int(rows[0]["n"]) if rows and "n" in rows[0] else 0

        # --- 2. nearest known planet (by sy_dist, parsecs) -----------
        # Avoid `select top 1 ... order by sy_dist asc` — it's broken
        # on pscomppars (returns ~493 pc as #1, misses Proxima at
        # 1.30 pc). Use a min() sub-select instead; the result set is
        # tiny (the nearest distance is shared by Proxima Cen b + d,
        # so we get 1–2 rows) and we pick the first deterministically.
        rows = _tap_get(
            "select pl_name, sy_dist from pscomppars "
            "where sy_dist = (select min(sy_dist) from pscomppars "
            "where sy_dist is not null)"
        )
        if not rows or "pl_name" not in rows[0]:
            return {"_error": "exoplanet nearest: empty or malformed response"}
        # Pick the alphabetically-first name from the tied set so the
        # firmware doesn't see the planet flip between "Proxima Cen b"
        # and "Proxima Cen d" depending on database ordering whims
        # (would re-seed the procedural planet for no good reason).
        rows_sorted = sorted(rows, key=lambda r: str(r.get("pl_name", "")))
        nearest_name_raw = str(rows_sorted[0]["pl_name"])
        sy_dist_pc       = float(rows_sorted[0]["sy_dist"])
        nearest_ly       = round(sy_dist_pc * _PC_TO_LY, 2)

        # Wire validation mirrors the firmware (handle_exoplanet):
        # 1..23 ASCII chars. The archive normally returns short names
        # like "Proxima Cen b" (13 chars); a future renaming campaign
        # could exceed the cap, in which case we want a graceful drop
        # on this end rather than a per-publish reject on-device.
        nearest_name = nearest_name_raw[:23]
        if not nearest_name or not all(
                32 <= ord(c) <= 126 for c in nearest_name):
            return {"_error":
                    f"exoplanet nearest_name failed wire check: {nearest_name_raw!r}"}

        return {
            "total_count":         total_count,
            "ytd_count":           ytd_count,
            "nearest_name":        nearest_name,
            "nearest_distance_ly": max(0.0, min(6553.5, nearest_ly)),
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
        # the cache for the next planet/moon trigger.
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
        # REST sensor polls every 30 s.
        #
        # Staleness guard — this is the whole reason this block is
        # paranoid. When WTIA's API is unreachable HA marks
        # sensor.iss_position as "unavailable", but the json_attributes
        # from the last good poll often stay attached to the entity,
        # so a naive `state.getattr(...)` happily returns ISS coords
        # that may be minutes-to-hours old. Republishing those at
        # 30 s gets the firmware cached for its full kFreshMs=1h
        # window; the ISS moves ~28 000 km in an hour, more than
        # enough to drift a real overhead pass into a phantom
        # near-horizon point that still satisfies the on-device
        # `elevation_deg ≥ 0 AND sun ≤ -6°` predicate — and the
        # auto-switch + ta-da fires on bogus data ("VIS 350x0" at
        # the horizon with no actual station there).
        #
        # We gate on two things:
        #   1. The entity STATE (not its attrs) must be a real
        #      number. HA writes "unavailable" / "unknown" to the
        #      state on poll failure; the attrs dict is unreliable.
        #   2. The entity's last_updated timestamp must be within
        #      90 s (≥ 2× scan_interval) — covers a single missed
        #      poll, fails closed on anything longer.
        state_val = state.get("sensor.iss_position")
        if state_val in (None, "", "unknown", "unavailable"):
            return {"_error": f"sensor.iss_position state={state_val!r}"}
        try:
            float(state_val)  # cheap "is this a real number?" check
        except (TypeError, ValueError):
            return {"_error": f"sensor.iss_position state not numeric: {state_val!r}"}

        # last_updated is exposed by pyscript as a dotted attribute
        # on the state object (`state.<entity>.last_updated`) and is
        # a tz-aware datetime. Fall back gracefully if pyscript can't
        # produce one — better to skip a publish than to publish stale.
        try:
            last_upd = state.get("sensor.iss_position.last_updated")
            from datetime import datetime, timezone
            age_s = (datetime.now(timezone.utc) - last_upd).total_seconds()
        except Exception as exc:  # noqa: BLE001
            return {"_error": f"could not read sensor.iss_position.last_updated: {exc}"}
        if age_s > 90.0:
            return {"_error": f"sensor.iss_position stale (age={age_s:.0f}s)"}

        attrs = state.getattr("sensor.iss_position")
        if not attrs:
            return {"_error": "sensor.iss_position has no attributes"}
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


# ---------------------------------------------------------------------
# Next-launch fetcher (FR-14.6, phase L) — pure HTTP + dict-shaping,
# no skyfield. Talks to The Space Devs' Launch Library 2 (LL2) free
# `launches/upcoming` endpoint and shapes the soonest pending entry
# into the firmware launch_state.h schema. Per FR-14 architectural
# rule, HA does NO observer-frame logic — we just pass through the
# schedule plus table-driven name compaction; the firmware does all
# T-minus math.
#
# Why LL2 (was: RocketLaunch.Live fdo `next/5`):
#   * LL2's `net` field is the official liftoff time and is always
#     populated. RLL's `t0` was frequently empty for Starship/TBD
#     missions, forcing the publisher onto `win_open` (~30 min earlier
#     than liftoff for SpaceX), producing a silent off-by-window
#     countdown vs SpaceX's own page.
#   * LL2 ships `launch_service_provider.abbrev` (e.g. "SpX", "RKLB",
#     "ULA") so we no longer need a hand-curated provider dict.
#   * `net_precision.abbrev` ("SEC"/"MIN"/"HR"/"DAY"/...) is an
#     honest estimate flag — set t0_estimate when precision is HR
#     or coarser.
#   * Free tier: 15 req/h anon (we poll once an hour at :07, so 14
#     spare). 1000/h with a free API key if we ever need bursts.
#   * `lldev.thespacedevs.com` (no-SLA dev mirror) is the same
#     schema; swap the host for local testing without burning quota.
#
# LL2 wire fields we read (mode=normal):
#   net                         — required, ISO-8601 UTC liftoff time
#   net_precision.abbrev        — "SEC" | "MIN" | "HR" | "DAY" | ...
#   window_end                  — optional, ISO-8601 UTC window close
#   status.id / status.abbrev   — 1=Go, 2=TBD, 3=Success, 4=Failure,
#                                 6=In Flight, 7=Partial Failure,
#                                 8=TBC. We only consider 1, 2, 8.
#   launch_service_provider.abbrev — e.g. "SpX" → upcased to "SPX"
#   rocket.configuration.name   — e.g. "Starship"
#   mission.name                — e.g. "Flight 12"
#   pad.location.name           — e.g. "SpaceX Starbase, TX, USA"
# ---------------------------------------------------------------------

# LL2's upcoming-launches endpoint. `hide_recent_previous=true` drops
# the past-12 h "Success/Failure" entries the bare endpoint includes
# for context, so we don't have to filter them ourselves. `mode=normal`
# is the middle response detail tier — gives us
# provider/rocket/mission/pad without dragging the 50 KB `detailed`
# tree (which includes wiki blurbs, full agency stats, etc).
_LAUNCH_URL = ("https://ll.thespacedevs.com/2.3.0/launches/upcoming/"
               "?limit=5&hide_recent_previous=false&mode=normal")

# LL2 launch status IDs we treat as upcoming. Anything else means the
# launch has already happened (3/4/6/7) or is on hold (5=Hold). TBD
# and TBC still get on screen so the operator sees what's queued.
_LL2_UPCOMING_STATUS = frozenset({1, 2, 8})  # Go, TBD, TBC

# LL2 statuses past t0 that we keep on the panel for `_POST_T0_HOLD_S`
# seconds after liftoff so the operator sees how the launch resolved
# instead of the scene silently swapping to the next upcoming entry
# the moment the odometer reaches T-0. Mapping into the firmware's
# `result` field (see include/state/launch_state.h):
#   3 Success         → result = 1
#   4 Failure         → result = 0
#   6 In Flight       → result = -1  (firmware infers "in flight" from
#                                     now > t0 + result == -1)
#   7 Partial Failure → result = 2
_LL2_POST_T0_STATUS = frozenset({3, 4, 6, 7})
_LL2_STATUS_TO_RESULT = {3: 1, 4: 0, 7: 2}  # 6 stays -1

# How long after t0 to keep a post-t0 entry selected before falling
# back to the soonest upcoming launch. 30 min is long enough to
# resolve outcome for most missions (LL2 typically flips Success
# within a few minutes of orbit insertion) and short enough that an
# extended in-flight phase won't crowd out a back-to-back launch.
_POST_T0_HOLD_S = 30 * 60

# `net_precision.abbrev` values that count as "exact enough to not
# bother surfacing as an estimate". SEC = accurate to the second
# (typical for SpaceX & Rocket Lab on launch day). MIN = good enough
# — that's a ±60 s ambiguity, smaller than the typical hold/scrub.
# Anything else (HR, DAY, MONTH, QUARTER, YEAR) is an estimate and
# the panel paints `~T` instead of `T-`.
_LL2_EXACT_PRECISION = frozenset({"SEC", "MIN"})

# Pad location name → ≤4-char tag for the firmware display. LL2 gives
# us `pad.location.name` as a human-readable string ("SpaceX Starbase,
# TX, USA"); we map common ones to the same 3-4 char codes
# RocketLaunch.Live's slug table used so on-panel pad codes stay
# stable across the source swap. Unknown locations fall through to
# `_derive_pad_code()` which slugifies + truncates.
_LL2_PAD_ABBREV = {
    "SpaceX Starbase, TX, USA":                       "STR",
    "Cape Canaveral SFS, FL, USA":                    "CCS",
    "Kennedy Space Center, FL, USA":                  "KSC",
    "Vandenberg SFB, CA, USA":                        "VSF",
    "Wallops Flight Facility, VA, USA":               "WAL",
    "Mid-Atlantic Regional Spaceport, VA, USA":       "MAR",
    "Mahia Peninsula, New Zealand":                   "LC1",
    "Wallops Flight Facility, Virginia, USA":         "WAL",
    "Baikonur Cosmodrome, Republic of Kazakhstan":    "BAI",
    "Plesetsk Cosmodrome, Russian Federation":        "PLE",
    "Vostochny Cosmodrome, Russian Federation":       "VOS",
    "Guiana Space Centre, French Guiana":             "KRU",
    "Satish Dhawan Space Centre, India":              "SDS",
    "Tanegashima Space Center, Japan":                "TNG",
    "Uchinoura Space Center, Japan":                  "UCH",
    "Wenchang Space Launch Site, People's Republic of China": "WEN",
    "Xichang Satellite Launch Center, People's Republic of China": "XIC",
    "Jiuquan Satellite Launch Center, People's Republic of China": "JIU",
    "Taiyuan Satellite Launch Center, People's Republic of China": "TAI",
}


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _derive_pad_code(loc_name):
    """Fallback ≤4-char ALLCAPS pad code when the location isn't in
    _LL2_PAD_ABBREV. Takes the first two whitespace-separated words
    of `loc_name` and concatenates their leading letters, e.g.
    'New Glenn Pad' → 'NGP'. Single-word locations fall back to the
    first 3 alpha letters."""
    if not loc_name:
        return "?"
    parts = [p for p in loc_name.replace(",", " ").split() if p]
    if not parts:
        return "?"
    if len(parts) == 1:
        head = "".join(c for c in parts[0] if c.isalpha()).upper()
        return head[:3] if head else "?"
    initials = "".join(p[0] for p in parts[:3] if p and p[0].isalpha()).upper()
    return initials[:4] if initials else "?"


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _shape_provider(abbrev, full_name):
    """Provider tag, ≤11 chars + NUL (kProviderCap-1). LL2 ships a
    pre-computed `abbrev` we just uppercase + clip; if it's missing
    we fall back to the first 3 alpha letters of the full name."""
    if abbrev:
        return abbrev.upper()[:11]
    if full_name:
        head = "".join(c for c in full_name.split()[0] if c.isalpha()).upper()
        return head[:3] if head else "?"
    return "?"


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _shape_org(full_name):
    """Full provider/organisation name, ≤20 chars + NUL (kOrgCap-1).
    Used by the typewriter info row's `ORG:` slide so the operator
    sees the company name (\"SPACEX\", \"ROCKET LAB\") in addition to
    the compact 2-letter `provider` abbrev. Empty string when LL2
    doesn't supply a name."""
    if not full_name:
        return ""
    # Drop common legal-suffix noise so \"SpaceX, Inc.\" doesn't burn
    # 5 chars on chrome. Conservative \u2014 only strip the trailing
    # token, never substrings.
    s = full_name.strip()
    for suffix in (", Inc.", ", Inc", " Inc.", " Inc",
                   ", LLC", " LLC", ", S.A.", " S.A.", " GmbH"):
        if s.endswith(suffix):
            s = s[: -len(suffix)].rstrip()
            break
    return s.upper()[:20]


# US country tokens we treat as "this is the United States" — the
# country segment is the last comma-separated piece of LL2's
# `pad.location.name`. ALLCAPS for case-insensitive compare against
# `.upper()`. Includes the common short and long forms LL2 has been
# observed to emit.
_US_COUNTRY_TOKENS = frozenset({
    "USA",
    "UNITED STATES",
    "UNITED STATES OF AMERICA",
})


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _shape_pad_country(loc_name):
    """Country (or \"STATE, USA\") slug for the typewriter info row,
    ≤20 chars + NUL (kPadCountryCap-1). LL2's `pad.location.name` is
    full \"City, STATE, COUNTRY\" form for US sites and \"City,
    COUNTRY\" for everything else (e.g. \"Cape Canaveral SFS, FL,
    USA\" vs \"Mahia Peninsula, New Zealand\"). We surface the
    country alone for international sites (the operator already gets
    the city via PAD context elsewhere; country is the navigation-
    map-level cue) and \"STATE, USA\" for US sites so the half-dozen
    Florida/California/Texas pads stay distinguishable on a glance.
    Empty string on absent input."""
    if not loc_name:
        return ""
    parts = [p.strip() for p in loc_name.split(",") if p.strip()]
    if not parts:
        return ""
    country = parts[-1]
    if country.upper() in _US_COUNTRY_TOKENS and len(parts) >= 3:
        state = parts[-2]
        result = f"{state}, USA"
    else:
        result = country
    return result.upper()[:20]


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _shape_vehicle(name):
    """Vehicle ALLCAPS, clipped to 11 chars (kVehicleCap-1)."""
    if not name:
        return "?"
    return name.upper()[:11]


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _shape_mission(name):
    """Mission ALLCAPS, clipped to 13 chars (kMissionCap-1) with a
    couple of common-case compactions so that 'Starlink Group 17-42'
    and 'Flight 12' read sensibly inside the budget.
    """
    if not name:
        return "?"
    s = name.strip()
    # "Starlink Group 17-42" / "Starlink 17-42" → "STARLINK 17-42"
    # (the firmware will then clip if still over budget).
    s = s.replace("Group ", "")
    # "Flight 12" → "FL12" (Starship test flights).
    if s.lower().startswith("flight "):
        tail = s.split(" ", 1)[1].strip()
        s = "FL" + tail
    # Strip trailing parenthetical suffixes — "Demo-2 (Crewed)" → "Demo-2"
    if "(" in s:
        s = s.split("(", 1)[0].strip()
    return s.upper()[:13]


# Firmware launch_state::kDescriptionCap - 1 = 240 chars usable.
# Keep this constant in sync with include/state/launch_state.h.
_DESCRIPTION_CAP = 240


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _shape_description(text):
    """Normalise LL2's `mission.description` prose to something the
    firmware's 6×8 mono marquee can render cleanly.

    Steps (in order):
      1. NFKD-decompose + drop combining marks → "café" → "cafe".
         The firmware uses Adafruit_GFX's built-in font which only
         covers ASCII 0x20..0x7E; a stray U+00E9 would render as the
         block-character placeholder and the marquee would look like
         it ate something.
      2. Replace control chars (CR, LF, TAB, BS, …) with a single
         space — LL2 prose contains literal '\\r\\n' line breaks that
         on the marquee read as garbage glyphs.
      3. Collapse runs of whitespace to a single space + strip ends.
      4. Drop any remaining non-ASCII / non-printable bytes.
      5. Hard-clip to _DESCRIPTION_CAP. If the clip lands mid-word,
         walk back to the last space so the marquee never ends mid-
         token; on clip we replace the final 3 chars with "..." so
         the reader knows the prose was truncated. (firmware accepts
         only up to kDescriptionCap-1 and rejects the whole publish
         if exceeded, so the cap must be enforced HA-side.)
    Returns "" on empty/None input."""
    if not text:
        return ""
    try:
        import unicodedata
        # 1. ASCII-fold via NFKD + drop combining marks.
        decomposed = unicodedata.normalize("NFKD", text)
        ascii_text = decomposed.encode("ascii", "ignore").decode("ascii")
        # 2 + 3. Replace control chars with spaces, collapse runs.
        cleaned_chars = []
        for ch in ascii_text:
            o = ord(ch)
            if o < 0x20 or o == 0x7F:
                cleaned_chars.append(" ")
            elif o > 0x7E:
                continue  # belt + braces; .encode('ascii','ignore') already dropped these
            else:
                cleaned_chars.append(ch)
        s = "".join(cleaned_chars)
        # Collapse whitespace and strip.
        s = " ".join(s.split())
        if not s:
            return ""
        # 5. Clip with word-boundary preservation + ellipsis.
        if len(s) <= _DESCRIPTION_CAP:
            return s
        # Reserve 3 chars for the ellipsis indicator.
        budget = _DESCRIPTION_CAP - 3
        cut = s.rfind(" ", 0, budget)
        if cut < budget // 2:  # don't lose more than half to a word boundary
            cut = budget
        return s[:cut].rstrip() + "..."
    except Exception:  # noqa: BLE001 — never break the publish over cosmetics
        return ""


@pyscript_compile  # called from @pyscript_executor _compute_launch — must be native
def _parse_iso_local_epoch(s, tz_name):
    """Parse an ISO-8601 timestamp (UTC, 'Z' or '+00:00' suffix) and
    return the Unix epoch of that instant **as expressed in HA's
    local clock** (the tz named by `tz_name`, e.g.
    "America/Los_Angeles").

    Rationale: the firmware's RTC stores local time and every
    countdown is `wire_epoch - tod::now().local_epoch`, so the wire
    contract for time-bearing payloads is *local epoch* (= UTC epoch
    shifted by HA's tz offset). HA owns the tz and is the only place
    that needs to know it; the device does pure subtraction.

    IMPORTANT: we explicitly resolve the tz from HA's configured
    `hass.config.time_zone` (passed in here as `tz_name`) instead of
    relying on `datetime.astimezone()` with no argument — the latter
    uses the Python process's `TZ` env, which inside the HA /
    pyscript container is frequently UTC even when HA itself is
    configured for a different zone.

    Returns None on parse failure."""
    if not s:
        return None
    try:
        from datetime import datetime, timezone
        try:
            from zoneinfo import ZoneInfo
        except ImportError:  # pragma: no cover — Python <3.9
            from backports.zoneinfo import ZoneInfo  # type: ignore
        # Python ≥3.11 accepts 'Z' directly via fromisoformat, but
        # 3.10 (HA's typical pyscript runtime) does not.
        normalised = s.strip().replace("Z", "+00:00")
        dt = datetime.fromisoformat(normalised)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        zone = ZoneInfo(tz_name) if tz_name else None
        # Convert to HA's configured zone, drop tz, then re-stamp as
        # UTC so .timestamp() returns (real_utc_epoch + tz_offset_s)
        # — i.e. a synthetic epoch whose integer value equals the
        # wall-clock seconds-since-1970 of the HA-local zone.
        local_naive = (dt.astimezone(zone) if zone is not None
                       else dt.astimezone()).replace(tzinfo=None)
        return int(local_naive.replace(tzinfo=timezone.utc).timestamp())
    except Exception:  # noqa: BLE001
        return None


@pyscript_executor
def _compute_launch(tz_name):
    """Fetch LL2 upcoming launches and pick the soonest pending entry,
    returning a dict matching the observatory/launch wire schema (see
    docs/MQTT_TOPICS.md). Returns `{"_error": "..."}` on any network
    or parse failure — `_publish` records that on the witness entity
    and the firmware keeps the previous fresh snapshot until the next
    hourly retry.

    `tz_name` MUST be HA's configured `hass.config.time_zone` (e.g.
    "America/Los_Angeles"). The pyscript container's TZ env can
    differ from HA's configured zone (commonly UTC inside the
    container while HA itself is local), and using the container's
    zone here produced a silent tz-offset error on the countdown
    wire. The caller resolves the zone and passes it explicitly so
    this function stays a pure executor with no HA-global access.

    Selection rules (two-pass, post-t0 hold wins over upcoming):
      Pass A — post-t0 hold (FR-14.6 "don't leave the operator
         guessing what happened"): scan for entries whose effective
         t0 sits in [now - `_POST_T0_HOLD_S`, now + 60 s] AND whose
         status is one of `_LL2_POST_T0_STATUS` (Success/Failure/
         In Flight/Partial). Pick the most recent (largest t0) such
         entry; populate `result` via `_LL2_STATUS_TO_RESULT` (or -1
         for in-flight). The firmware paints the count-up T+ odometer
         and the typewriter row's RESULT slide off this payload.
      Pass B — next upcoming (the original flow):
        1. Filter to `status.id \u2208 _LL2_UPCOMING_STATUS`.
        2. Effective t0 = `net` (LL2 guarantees this is populated).
           No fallback path needed — if `net` is missing the entry
           is malformed and we skip it.
        3. `t0_estimate = net_precision.abbrev \u2209 {SEC, MIN}`. HR or
           coarser \u2192 on-panel `~T` prefix flag.
        4. Drop if effective t0 < now - 6 h. The 6 h grace covers a
           slipping window that the operator might still want to
           see post-instant.
        5. First survivor wins (API returns chronologically by `net`).
    """
    try:
        import urllib.request

        req = urllib.request.Request(
            _LAUNCH_URL,
            headers={
                # LL2 asks API consumers to identify themselves so
                # they can contact you if your traffic pattern is
                # ever a problem. The URL is informational only.
                "User-Agent": "quantum-observatory/1.0 (+https://github.com)",
                "Accept": "application/json",
            },
        )
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("utf-8", errors="replace")
        doc = json.loads(body)
        launches = doc.get("results") or []
        if not isinstance(launches, list) or not launches:
            return {"_error": "LL2 returned no upcoming launches"}

        # `now` in the same local-epoch frame as the parsed net values
        # below — wall-clock seconds-since-1970 of HA's local zone.
        # Resolve via HA's configured tz_name (not the container's TZ
        # env) so this matches what _parse_iso_local_epoch emits.
        from datetime import datetime as _dt, timezone as _tz
        try:
            from zoneinfo import ZoneInfo as _ZI
        except ImportError:  # pragma: no cover — Python <3.9
            from backports.zoneinfo import ZoneInfo as _ZI  # type: ignore
        _zone = _ZI(tz_name) if tz_name else None
        _now_utc = _dt.now(tz=_tz.utc)
        _now_local_naive = (_now_utc.astimezone(_zone) if _zone is not None
                            else _now_utc.astimezone()).replace(tzinfo=None)
        now = int(_now_local_naive.replace(tzinfo=_tz.utc).timestamp())

        entry_failures = []  # see RocketLaunch.Live version for rationale

        # Helper to shape one LL2 entry into the wire payload dict.
        # Kept inside _compute_launch so it closes over `tz_name` and
        # `entry_failures` and stays @pyscript_executor-pure (no HA
        # globals). The `result` arg is resolved by the caller from
        # the entry's LL2 status before this runs \u2014 Pass A pulls it
        # from `_LL2_STATUS_TO_RESULT`, Pass B always passes -1.
        def _shape_entry(entry, t0_epoch, result_val):
            precision = (entry.get("net_precision") or {}).get("abbrev", "")
            t0_estimate = precision not in _LL2_EXACT_PRECISION

            # Optional window close (LL2 ships `window_end` in the
            # same UTC ISO frame). Sanity-check it sits in
            # [t0, t0 + 24 h] so a botched schema doesn't enable
            # LIVE regime at a wildly wrong time.
            win_close_epoch = 0
            wc = _parse_iso_local_epoch(entry.get("window_end"), tz_name)
            if wc is not None and wc > t0_epoch and wc < t0_epoch + 86400:
                win_close_epoch = wc

            provider = entry.get("launch_service_provider") or {}
            rocket   = entry.get("rocket") or {}
            config   = rocket.get("configuration") or {}
            mission  = entry.get("mission") or {}
            pad      = entry.get("pad") or {}
            pad_loc  = (pad.get("location") or {}).get("name", "")

            return {
                "t0_local_epoch":              t0_epoch,
                "t0_estimate":                 bool(t0_estimate),
                "t0_window_close_local_epoch": win_close_epoch,
                "provider": _shape_provider(provider.get("abbrev"),
                                            provider.get("name")),
                "vehicle":  _shape_vehicle(config.get("name")),
                "mission":  _shape_mission(mission.get("name")
                                           or entry.get("name", "")),
                "pad_code": _LL2_PAD_ABBREV.get(pad_loc)
                            or _derive_pad_code(pad_loc),
                "org":         _shape_org(provider.get("name")),
                "pad_country": _shape_pad_country(pad_loc),
                "description": _shape_description(
                    mission.get("description", "")),
                "result":   int(result_val),
            }

        # ---- Pass A: post-t0 hold (FR-14.6 outcome visibility) ----
        # Walk all entries, collect the post-t0 candidates whose t0
        # sits within the hold window. Pick the largest t0 (= most
        # recent liftoff) \u2014 LL2 may interleave past/future when
        # `hide_recent_previous=false` and we want the freshest
        # outcome on screen, not a stale one from 28 minutes ago
        # if a newer launch already happened.
        post_t0_best = None  # (t0_epoch, status_id, entry)
        for idx, entry in enumerate(launches):
            try:
                status_id = (entry.get("status") or {}).get("id")
                if status_id not in _LL2_POST_T0_STATUS:
                    continue
                t0_epoch = _parse_iso_local_epoch(entry.get("net"), tz_name)
                if t0_epoch is None:
                    continue
                # +60 s upper bound covers clock-skew between HA and
                # LL2 \u2014 a launch whose `net` lands a few seconds in
                # the future but whose status already reads "Success"
                # is post-t0 in reality, not upcoming.
                if t0_epoch < now - _POST_T0_HOLD_S or t0_epoch > now + 60:
                    continue
                if post_t0_best is None or t0_epoch > post_t0_best[0]:
                    post_t0_best = (t0_epoch, status_id, entry)
            except Exception as exc:  # noqa: BLE001 \u2014 per-entry guard
                entry_failures.append(
                    f"entry[{idx}] (post-t0 scan): "
                    f"{type(exc).__name__}: {exc}"
                )
                continue

        if post_t0_best is not None:
            t0_epoch, status_id, entry = post_t0_best
            result_val = _LL2_STATUS_TO_RESULT.get(status_id, -1)
            try:
                payload = _shape_entry(entry, t0_epoch, result_val)
                if entry_failures:
                    payload["_warnings"] = entry_failures
                return payload
            except Exception as exc:  # noqa: BLE001
                entry_failures.append(
                    f"post-t0 shape failed: {type(exc).__name__}: {exc}"
                )
                # Fall through to upcoming-pass on shape failure so
                # the panel still has *something* fresh on it.

        # ---- Pass B: soonest upcoming (existing flow) ----
        for idx, entry in enumerate(launches):
            try:
                status = entry.get("status") or {}
                if status.get("id") not in _LL2_UPCOMING_STATUS:
                    continue

                t0_epoch = _parse_iso_local_epoch(entry.get("net"), tz_name)
                if t0_epoch is None:
                    continue
                if t0_epoch < now - 6 * 3600:
                    continue

                payload = _shape_entry(entry, t0_epoch, -1)
                if entry_failures:
                    payload["_warnings"] = entry_failures
                return payload
            except Exception as exc:  # noqa: BLE001 \u2014 per-entry guard
                entry_failures.append(
                    f"entry[{idx}]: {type(exc).__name__}: {exc}"
                )
                continue

        return {
            "_error": "no upcoming launches in LL2 window",
            "_warnings": entry_failures,
        }
    except Exception as exc:  # noqa: BLE001
        return {"_error": f"{type(exc).__name__}: {exc}"}


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
    # Slug: "observatory/planet" → "observatory_publisher_planet".
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
    # 24 h for planet/moon/constellation). One PUBACK round-trip per
    # ≤hourly publish is a trivial cost for guaranteed delivery.
    # Matches the qos:1 setting on every YAML mqtt.publish in
    # ../packages/quantum_observatory.yaml.
    retain = topic in _RETAIN_TOPICS
    service.call(
        "mqtt", "publish",
        topic=topic, payload=payload, retain=retain, qos=1,
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
def publish_planet(name="jupiter", **_):
    """Active planet — every 15 min. Firmware kFreshMs = 1 h, so we
    have 4× headroom for missed publishes.

    `name` selects the body to ephemeris; defaults to 'jupiter' so
    the cron tick has something to publish without per-tick state.
    Operators / automations can call this service with a different
    name (e.g. 'mars', 'saturn') to overlay live look-angles on the
    `planets` scene's current cursor when it lands on that body.
    Bodies outside _SKYFIELD_BODY_BY_NAME (e.g. 'titan', 'europa')
    are not supported here — the catalog's static fact line keeps
    rendering for those.

    Also exposed as service `pyscript.publish_planet` so
    homeassistant/setup_mqtt.py --verify-publisher can force-call it."""
    lat, lon = _observer_lat_lon()
    _publish("observatory/planet", _compute_planet(lat, lon, name))


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
@time_trigger("startup", "cron(*/10 * * * *)")
def publish_launch(**_):
    """Next-scheduled rocket launch — every 10 min.

    Cadence rationale: the post-t0 hold window (`_POST_T0_HOLD_S` =
    30 min) needs to see LL2's status flip from In Flight \u2192 Success
    /Failure/Partial soon enough to land on the panel before the
    hold expires and the scene moves on to the next upcoming entry.
    Hourly was fine when the only payload was the next-upcoming
    countdown (4 h kFreshMs gave 4\u00d7 headroom); 10 min keeps the
    result slide accurate to within ~10 min of LL2 publishing the
    outcome. 6 calls/hour is well under LL2's 15/hr unauthenticated
    throttle.

    Also exposed as service `pyscript.publish_launch`."""
    # Resolve HA's configured tz_name here (the @service body has the
    # pyscript-injected `hass` global) and pass it into the executor.
    # _compute_launch can't read hass.config itself — @pyscript_executor
    # functions run as native Python on a worker thread without
    # pyscript globals.
    try:
        tz_name = str(hass.config.time_zone) if hass.config.time_zone else None
    except Exception:  # noqa: BLE001
        tz_name = None
    result = _compute_launch(tz_name)
    # Drain the side-channel diagnostics _compute_launch could not log
    # itself (it's @pyscript_executor, so `log` is unavailable there).
    # Pop them BEFORE handing off to _publish so they don't end up on
    # the MQTT wire or the witness entity's attributes.
    warnings = result.pop("_warnings", None) if isinstance(result, dict) else None
    if warnings:
        for w in warnings:
            log.warning(f"observatory_publisher: launch parse: {w}")
    _publish("observatory/launch", result)


# ---------------------------------------------------------------------
# Exoplanet rolling-history state — persists across HA restarts via
# pyscript's `state.set` (which writes to /config/.storage/pyscript).
# Stored as a single state entity (`pyscript.exoplanet_history`) with
# the most-recent snapshot list in an attribute. Keeps the last ~9
# days of (iso_time, count) tuples; trims older entries on each push.
# ---------------------------------------------------------------------

_EXO_HISTORY_ENTITY = "pyscript.exoplanet_history"
_EXO_HISTORY_KEEP_DAYS  = 9     # prune anything older
_EXO_DELTA_TARGET_DAYS  = 7     # delta vs the snapshot closest to this age
_EXO_DELTA_MIN_AGE_DAYS = 5     # ...but only if at least this old


def _exoplanet_added_recent(current_total, ytd_count=0):
    """Maintain rolling history; return the 7-day delta (or None).

    Reads `pyscript.exoplanet_history`'s `snapshots` attribute, picks
    the entry whose age is closest to 7 d (and ≥ 5 d), computes delta,
    appends the current snapshot, prunes to the keep window, and
    writes back. Returns None when there is no eligible historical
    snapshot yet AND no bootstrap estimate is available.

    First-run bootstrap (FR-UX): when history is empty, synthesise a
    single back-dated snapshot at `now - 7 d` whose value is
    `current_total - est_7d`, where `est_7d = round(ytd_count *
    7 / day_of_year)` extrapolates the year-to-date discovery pace.
    The firmware then sees a real, statistically-grounded weekly
    delta on day 1 instead of the +0 WK placeholder. Subsequent
    daily runs append real snapshots; once any real entry is closer
    in age to 7 d than the synthetic one (day 8+), the synthetic
    entry is pruned automatically by the 9-day keep window and the
    delta reflects measured truth from then on. This means: days
    1–7 show extrapolated pace, days 8+ show measured delta.
    """
    from datetime import datetime, timezone, timedelta

    now_utc = datetime.now(timezone.utc)

    snapshots = []
    try:
        attrs = state.getattr(_EXO_HISTORY_ENTITY) or {}
        snapshots = list(attrs.get("snapshots", []))
    except Exception:  # noqa: BLE001 — first-ever run, entity missing
        snapshots = []

    # --- bootstrap: synthesise a 7d-ago entry when no usable history --
    # Fires on first-ever run AND on runs where the only existing
    # snapshots are too young (< MIN_AGE) to compute a delta from \u2014
    # i.e. the bootstrap window is "no eligible historical entry
    # exists yet", not just "the entity doesn't exist". Without this
    # the second condition, a user who triggers the publisher twice
    # on day 1 would still see +0 WK (the first publish stored a
    # today-dated entry which then suppresses seeding).
    has_eligible = False
    for entry in snapshots:
        if not (isinstance(entry, (list, tuple)) and len(entry) == 2):
            continue
        try:
            age_d = (now_utc
                     - datetime.fromisoformat(entry[0])).total_seconds() / 86400.0
            if age_d >= _EXO_DELTA_MIN_AGE_DAYS:
                has_eligible = True
                break
        except Exception:  # noqa: BLE001
            continue
    if not has_eligible and int(ytd_count) > 0:
        day_of_year = now_utc.timetuple().tm_yday
        if day_of_year >= 7:
            est_7d = int(round(int(ytd_count) * 7.0 / day_of_year))
        else:
            # Very early January \u2014 not enough YTD signal yet. Fall
            # back to the prior-year-average weekly pace by
            # extrapolating ytd over the full year then /52.
            est_7d = int(round(int(ytd_count) / 52.0)) if ytd_count else 0
        if est_7d != 0:
            seed_ts = now_utc - timedelta(days=7)
            seed_val = max(0, int(current_total) - est_7d)
            snapshots.append([seed_ts.isoformat(), seed_val])

    delta = None
    best_gap = None
    for entry in snapshots:
        if not (isinstance(entry, (list, tuple)) and len(entry) == 2):
            continue
        iso_s, count_s = entry
        try:
            ts = datetime.fromisoformat(iso_s)
            age_d = (now_utc - ts).total_seconds() / 86400.0
        except Exception:  # noqa: BLE001 — corrupt entry, skip
            continue
        if age_d < _EXO_DELTA_MIN_AGE_DAYS:
            continue
        gap = abs(age_d - _EXO_DELTA_TARGET_DAYS)
        if best_gap is None or gap < best_gap:
            best_gap = gap
            delta = int(current_total) - int(count_s)

    # Append current snapshot + prune entries older than the keep window.
    snapshots.append([now_utc.isoformat(), int(current_total)])
    keep_after = now_utc - timedelta(days=_EXO_HISTORY_KEEP_DAYS)
    pruned = []
    for entry in snapshots:
        if not (isinstance(entry, (list, tuple)) and len(entry) == 2):
            continue
        try:
            if datetime.fromisoformat(entry[0]) >= keep_after:
                pruned.append(list(entry))
        except Exception:  # noqa: BLE001
            continue

    state.set(_EXO_HISTORY_ENTITY, str(int(current_total)),
              new_attributes={"snapshots": pruned})

    return delta


@service
@time_trigger("startup", "cron(17 6 * * *)")
def publish_exoplanet(**_):
    """Exoplanet count — daily at 06:17 local (random-ish minute to
    be polite to the NASA archive). Firmware kFreshMs = 48 h, so a
    missed publish doesn't blank the scene.

    Computes the 7-day rolling delta from the persisted history
    snapshots; omits `added_recent` on the wire until ≥ 5 days of
    history exists (so the firmware renders the "+0 WK" placeholder
    rather than lying with a 1-day delta in the first week).

    Also exposed as service `pyscript.publish_exoplanet` so
    homeassistant/setup_mqtt.py --verify-publisher can force-call it.
    """
    result = _compute_exoplanet()
    if isinstance(result, dict) and "_error" not in result:
        # ytd_count is publisher-internal; strip it before passing the
        # dict to _publish so it doesn't leak onto the MQTT wire (the
        # firmware schema doesn't know the field and would reject the
        # payload if it grew past the 256 B route-table cap).
        ytd = int(result.pop("ytd_count", 0))
        delta = _exoplanet_added_recent(result.get("total_count", 0), ytd)
        if delta is not None:
            result["added_recent"] = int(delta)
    _publish("observatory/exoplanet", result)


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

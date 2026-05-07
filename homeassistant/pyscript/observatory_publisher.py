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
    """Wrapper around mqtt.publish that logs the outcome."""
    if "_error" in payload_dict:
        log.error(f"observatory_publisher: {topic} skipped: {payload_dict['_error']}")
        return
    payload = json.dumps(payload_dict, separators=(",", ":"))
    service.call(
        "mqtt", "publish",
        topic=topic, payload=payload, retain=False,
    )
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

#!/usr/bin/env python3
"""Validate Home Assistant + MQTT setup for Quantum Observatory.

Usage
-----
    # one-shot validate (default — runs only the 5 read-only checks)
    python homeassistant/setup_mqtt.py \
        --url http://homeassistant.local:8123 \
        --token "$HA_TOKEN"

    # do EVERYTHING (install YAML, reload, verify, trigger, prove publish)
    python homeassistant/setup_mqtt.py --all \
        --url http://homeassistant.local:8123 \
        --token "$HA_TOKEN" \
        --ssh-host homeassistant.local --ssh-port 22

    # or via env (recommended — keeps secrets out of shell history)
    export HA_URL=http://homeassistant.local:8123
    export HA_TOKEN=eyJ...   # Long-Lived Access Token
    export HA_SSH_HOST=homeassistant.local
    python homeassistant/setup_mqtt.py --all

    # or via a .env file (no shell exports needed). The loader looks
    # in (in order) $CWD/.env, homeassistant/.env, and repo-root .env;
    # first hit wins. Real env vars always beat the file. Format is
    # plain `KEY=value` per line; `#` comments + blank lines + an
    # optional leading `export` are tolerated.
    cat > .env <<'EOF'
    HA_URL=http://homeassistant.local:8123
    HA_TOKEN=eyJ...
    HA_SSH_HOST=homeassistant.local
    HA_SSH_PORT=22
    EOF
    chmod 600 .env            # the file holds a long-lived token
    echo .env >> .gitignore   # never commit it
    python homeassistant/setup_mqtt.py --all

CLI flags (all also available as env vars)
------------------------------------------
Connection (always required):
  --url URL                HA base URL.                          [HA_URL]
  --token TOKEN            Long-Lived Access Token from
                           {url}/profile/security.               [HA_TOKEN]

Pipeline switches (all default off; any combination is fine):
  --install                SCP packages/quantum_observatory.yaml
                           to /config/packages/ on the HA host.
                           Requires SSH add-on with your public
                           key in authorized_keys.
  --install-publisher      SCP pyscript/observatory_publisher.py +
                           requirements.txt to /config/pyscript/.
                           Provides real Jupiter / moon /
                           constellation data via skyfield. Also
                           greps configuration.yaml for the needed
                           pyscript: block (paste it if missing).
  --reload                 Call homeassistant.reload_all (allow
                           up to 180 s — bigger HA installs take
                           a while).
  --verify-automations     Confirm all 3 automation.observatory_*
                           entities are registered. Looks them up
                           by their `id:` (HA derives the actual
                           entity_id from the slugified alias).
                           (Was 6 entities before phase 7.5.1; the
                           moon/jupiter/constellation YAML stubs
                           moved to the pyscript publisher.)
  --trigger-all            Force-fire every Observatory automation
                           now (skip_condition=true).
  --verify-published       Read each automation's last_triggered
                           and assert it ran ≤ 60 s ago.
  --verify-publisher       Force-call each pyscript publisher
                           (jupiter / moon / constellation) and
                           verify each one actually published by
                           reading the witness state entity
                           `pyscript.observatory_publisher_*` and
                           confirming `last_changed` advanced.
                           Closes the gap that --verify-published
                           can't see (pyscript triggers aren't HA
                           automation entities).
  --all                    Run --install → --install-publisher →
                           check_config → --reload →
                           --verify-automations → --trigger-all →
                           --verify-published → --verify-publisher
                           in order. If any
                           step fails the next is skipped.

SSH knobs (only used by --install / --all):
  --ssh-host HOST          Default = host portion of --url.      [HA_SSH_HOST]
  --ssh-user USER          Default = root (SSH add-ons run as
                           root).                                [HA_SSH_USER]
  --ssh-port PORT          Default = 22 (official Terminal & SSH
                           add-on). Use 22222 for the community
                           "Advanced SSH & Web Terminal" add-on. [HA_SSH_PORT]
  --ssh-key PATH           Default = ssh-agent / ~/.ssh/id_*.    [HA_SSH_KEY]

Behaviour notes
---------------
  * Idempotent: re-runs overwrite /config/packages/quantum_observatory.yaml
    (no merge, no backup) and reload picks up the latest. configuration.yaml
    is NEVER edited — the install step only greps it and prints an exact
    copy-paste block when the `packages: !include_dir_named packages`
    line is missing.
  * No HA UI needed beyond initial token + SSH-key setup.
  * Stdlib-only — no `pip install` required.

Exit codes
----------
  0  everything green
  1  bad input (missing URL/token, host unreachable, …)
  2  HA reachable but some step failed — actionable hints printed inline

What it does (read-only checks first, then optional pipeline)
-------------------------------------------------------------
  1. /api/                                  — token + URL reachable
  2. /api/config                            — HA Core ≥ 2026.2.2
  3. /api/config                            — `mqtt` is in `components`
                                              (= MQTT integration loaded)
  4. /api/hassio/info + /addons             — HA OS ≥ 17.1 + Mosquitto
                                              add-on installed and started
                                              (HA OS / Supervised only;
                                              skipped on Container/Core)
  5. service: mqtt.publish                  — round-trip a sentinel
                                              message to `observatory/status`

Optional no-UI pipeline (any combination of flags, or --all):
  6.   SCP package YAML → /config/packages/ (--install)
                                              also greps configuration.yaml
                                              and tells you the exact line
                                              to paste if missing
  6a.  /api/config                            — `pyscript` is in `components`
                                              (--install-publisher)
  6b.  SCP pyscript/observatory_publisher.py + requirements.txt
       → /config/pyscript/                    (--install-publisher)
                                              also greps configuration.yaml
                                              for `pyscript:` + allow_all_imports
                                              + hass_is_global; calls
                                              pyscript.reload on success
  7.   service: homeassistant.check_config    (always, before reload)
  8.   service: homeassistant.reload_all      (--reload)
  9.   /api/states filter by id attribute     (--verify-automations)
 10.   service: automation.trigger × 3        (--trigger-all)
 11.   /api/states/<entity> last_triggered    (--verify-published)
                                              checks ≤ 60 s ago
 11a.  service: pyscript.publish_* × 3 +      (--verify-publisher)
       /api/states/pyscript.observatory_*       proves each pyscript
       last_changed advance                     topic actually pushed

It deliberately does NOT try to create the MQTT integration via REST —
HA exposes config flows only through the WebSocket API, and creating
one needs the broker host/port/creds anyway. If step 2 fails the
script prints the exact two-click path through the UI to add the
integration (or, on HA OS, accept the auto-discovered Mosquitto
add-on broker).

Assumptions (the script tries to verify each):
  * Mosquitto broker add-on is installed (HA OS / Supervised only).
  * The standard `mqtt` integration is configured.
  * The provided token is a Long-Lived Access Token from a user
    account that has API access (any non-restricted user).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

# --- minimum supported versions ----------------------------------------
#
# Bumped when the YAML in ../packages/quantum_observatory.yaml uses a
# feature that's only stable from this version onward. Today the gate
# is the modern `triggers/conditions/actions` + `action:` (vs legacy
# `service:`) automation syntax — fully supported and warning-free
# from HA Core 2024.10, but we pin to 2026.2.2 so users running this
# script also get all the post-2024 MQTT discovery + REST sensor
# fixes the firmware contracts assume.
MIN_HA_CORE = (2026, 2, 2)   # Home Assistant Core
MIN_HA_OS   = (17, 1)        # Home Assistant OS (host); skipped on Container/Core

# --- expected unique IDs matching ../packages/quantum_observatory.yaml ---
#
# These are the `id:` values in each ACTIVE automation block. HA
# generates the actual entity_id from the slugified `alias`, NOT from
# `id:` — so we look up the real entity_id at runtime by matching the
# `id` attribute on every automation.* state. Keep this list in sync
# with the YAML.
#
# observatory_jupiter / observatory_moon / observatory_constellation
# used to live here too; they were superseded by the pyscript
# publisher (see ../pyscript/observatory_publisher.py) and commented
# out in the YAML. Re-add them here if you re-enable the YAML stubs.
EXPECTED_AUTOMATION_IDS = [
    "observatory_time",
    "observatory_evening_rotation",
]

# observatory_iss used to live in the list above; it was superseded
# by the pyscript publisher (publish_iss in observatory_publisher.py)
# after the open-notify /iss-pass.json endpoint went HTTP 404 and the
# YAML's sunlit = (visibility == "daylight") mapping was found to drop
# the dusk/dawn "visible" case. Re-add here if you re-enable the YAML
# automation in packages/quantum_observatory.yaml.

# Where the package goes inside HA's config directory.
HA_PACKAGE_DEST = "/config/packages/quantum_observatory.yaml"
# Local path to the YAML this script ships with.
HA_PACKAGE_SRC  = Path(__file__).parent / "packages" / "quantum_observatory.yaml"

# Pyscript publisher (replaces the moon / jupiter / constellation YAML
# stubs with real skyfield ephemeris). Both files are SCP'd as a unit
# to /config/pyscript/ when --install-publisher (or --all) is set.
HA_PYSCRIPT_DIR_DEST  = "/config/pyscript"
HA_PYSCRIPT_FILES_SRC = [
    Path(__file__).parent / "pyscript" / "observatory_publisher.py",
    Path(__file__).parent / "pyscript" / "requirements.txt",
]

# Each tuple is (pyscript service name, MQTT topic the function publishes
# on success). The service name matches the top-level `def` in
# pyscript/observatory_publisher.py — pyscript auto-registers every
# top-level function as `pyscript.<name>` and lets us call it via the
# standard /api/services/pyscript/<name> endpoint. The success log line
# we grep for is emitted by `_publish()` as:
#   observatory_publisher: <topic> → <payload>
PYSCRIPT_PUBLISHERS = [
    ("publish_jupiter",       "observatory/jupiter"),
    ("publish_moon",          "observatory/moon"),
    ("publish_constellation", "observatory/constellation"),
    # ISS publisher (replaces the YAML observatory_iss automation).
    # Combines WTIA + astros REST sensors with a skyfield-propagated
    # next-pass time from the CelesTrak TLE. The first publish after
    # pyscript reload may carry seconds_until_next=604800 (the doc's
    # max, ≈ "VIS IN 7D") if refresh_iss_next_pass hasn't finished
    # warming the cache yet — subsequent 30 s ticks pick up the real
    # value once skyfield + the TLE fetch complete (~5-15 s warm,
    # up to 90 s cold while DE421 downloads).
    ("publish_iss",           "observatory/iss"),
]


# --- tiny ANSI helpers (no external deps so this runs on a stock HA OS) ---

def _c(code: str, text: str) -> str:
    if not sys.stdout.isatty():
        return text
    return f"\033[{code}m{text}\033[0m"


def ok(msg: str) -> None:
    print(f"  {_c('32', 'OK')}   {msg}")


def warn(msg: str) -> None:
    print(f"  {_c('33', 'WARN')} {msg}")


def fail(msg: str) -> None:
    print(f"  {_c('31', 'FAIL')} {msg}")


def step(n: int | str, title: str) -> None:
    print()
    print(_c("1;36", f"[{n}] {title}"))


# --- version helpers ---------------------------------------------------

def parse_version(s: str) -> tuple[int, ...]:
    """Parse '2026.2.2' or '17.1' or '2026.2.2b3' to a comparable tuple.

    Strips any non-numeric suffix (beta/dev tags) so a pre-release of
    the target version still validates — we want to encourage testing
    on betas, not block it.
    """
    parts: list[int] = []
    for chunk in s.split("."):
        m = re.match(r"\d+", chunk)
        if not m:
            break
        parts.append(int(m.group()))
    return tuple(parts) if parts else (0,)


def fmt_version(v: tuple[int, ...]) -> str:
    return ".".join(str(x) for x in v)


# --- HTTP wrapper -------------------------------------------------------

class HA:
    def __init__(self, url: str, token: str, timeout: float = 10.0) -> None:
        self.url = url.rstrip("/")
        self.token = token
        self.timeout = timeout

    def _req(
        self,
        path: str,
        method: str = "GET",
        body: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> tuple[int, Any]:
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(
            f"{self.url}{path}",
            data=data,
            method=method,
            headers={
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/json",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
                try:
                    return resp.status, json.loads(raw) if raw else None
                except json.JSONDecodeError:
                    return resp.status, raw
        except urllib.error.HTTPError as e:
            raw = e.read().decode("utf-8", errors="replace") if e.fp else ""
            try:
                payload = json.loads(raw) if raw else None
            except json.JSONDecodeError:
                payload = raw
            return e.code, payload
        except urllib.error.URLError as e:
            raise RuntimeError(f"could not reach {self.url}: {e.reason}") from e

    def get(self, path: str) -> tuple[int, Any]:
        return self._req(path, "GET")

    def post(self, path: str, body: dict[str, Any], timeout: float | None = None) -> tuple[int, Any]:
        return self._req(path, "POST", body, timeout=timeout)


# --- individual checks --------------------------------------------------

def check_token(ha: HA) -> bool:
    step(1, "Verify token + URL reachable")
    code, body = ha.get("/api/")
    if code == 200 and isinstance(body, dict) and body.get("message") == "API running.":
        ok(f"connected to {ha.url}")
        return True
    if code == 401:
        fail("HTTP 401 — token rejected. Generate a Long-Lived Access Token at:")
        fail(f"    {ha.url}/profile/security  (scroll to bottom)")
        return False
    fail(f"unexpected response from /api/: HTTP {code} — {body!r}")
    return False


def check_core_version(ha: HA) -> tuple[bool, str | None]:
    """Returns (ok, raw_version_string).

    HA Core's version is reported by /api/config as e.g. '2026.2.2'.
    Anything older than MIN_HA_CORE fails hard — the YAML uses the
    modern automation syntax that's a deprecation warning on older
    cores and outright removed on much older ones.
    """
    step(2, f"Verify HA Core ≥ {fmt_version(MIN_HA_CORE)}")
    code, body = ha.get("/api/config")
    if code != 200 or not isinstance(body, dict):
        fail(f"could not read /api/config (HTTP {code})")
        return False, None
    raw = str(body.get("version", ""))
    if not raw:
        fail("/api/config did not include a `version` field")
        return False, None
    parsed = parse_version(raw)
    if parsed >= MIN_HA_CORE:
        ok(f"HA Core {raw} (≥ {fmt_version(MIN_HA_CORE)})")
        return True, raw
    fail(f"HA Core {raw} is older than required {fmt_version(MIN_HA_CORE)}.")
    fail("Update HA Core first (Settings → System → Updates) and re-run.")
    return False, raw


def check_mqtt_integration(ha: HA) -> bool:
    step(3, "Verify MQTT integration is loaded")
    code, body = ha.get("/api/config")
    if code != 200 or not isinstance(body, dict):
        fail(f"could not read /api/config (HTTP {code})")
        return False
    components = body.get("components", []) or []
    if "mqtt" in components:
        ok("`mqtt` component is loaded")
        return True
    fail("`mqtt` component is NOT loaded — the integration isn't configured.")
    print()
    print("  Two-click fix in the UI:")
    print(f"    1. Open  {ha.url}/config/integrations/dashboard")
    print("    2. Click 'Add Integration' → search 'MQTT' → pick the")
    print("       discovered Mosquitto broker (or fill host/port/user/pass).")
    print()
    print("  Once added, re-run this script.")
    return False


def check_supervisor_and_mosquitto(ha: HA) -> tuple[bool | None, str | None]:
    """Returns ((mosq_running, hassos_version_or_none)).

    First element: True=running, False=missing/stopped, None=no Supervisor.
    Second element: HA OS version string, or None on Container/Core/error.
    Logs a FAIL when HA OS is older than MIN_HA_OS but does NOT itself
    flip the bool — add-on state and host-OS version are independent
    failures and the summary surfaces both.
    """
    step(4, "Verify Mosquitto add-on + HA OS version (HA OS / Supervised only)")
    # /api/hassio/info gives us hassos + supervisor version in one call.
    code, body = ha.get("/api/hassio/info")
    hassos_version: str | None = None
    if code == 404:
        warn("no Supervisor on this install (HA Container or Core) — skipping.")
        warn("Make sure your broker is reachable independently.")
        return None, None
    if code in (401, 403):
        warn("Supervisor present but token can't read /api/hassio (needs the")
        warn("`hassio.admin` permission — usually means token belongs to a")
        warn("non-admin user). Skipping add-on + OS version check.")
        return None, None
    if code == 200 and isinstance(body, dict):
        data = body.get("data") or body
        hassos_version = data.get("hassos") or None
        if hassos_version:
            parsed = parse_version(hassos_version)
            if parsed >= MIN_HA_OS:
                ok(f"HA OS {hassos_version} (≥ {fmt_version(MIN_HA_OS)})")
            else:
                fail(f"HA OS {hassos_version} is older than required {fmt_version(MIN_HA_OS)}.")
                fail("Update from Settings → System → Updates and re-run.")
        else:
            warn("Supervisor reports no `hassos` version (Supervised on a")
            warn("generic OS) — skipping host-OS version check.")
    else:
        warn(f"unexpected /api/hassio/info response (HTTP {code}) — skipping OS check.")

    # Now the add-on listing.
    code, body = ha.get("/api/hassio/addons")
    if code != 200 or not isinstance(body, dict):
        warn(f"unexpected /api/hassio/addons response (HTTP {code}) — skipping add-on check.")
        return None, hassos_version

    addons = (body.get("data") or {}).get("addons") or body.get("addons") or []
    mosq = next(
        (a for a in addons if "mosquitto" in (a.get("slug", "") + a.get("name", "")).lower()),
        None,
    )
    if not mosq:
        fail("Mosquitto add-on not installed.")
        print(f"    Install it from {ha.url}/hassio/addon/core_mosquitto/info")
        return False, hassos_version

    state = mosq.get("state", "unknown")
    slug = mosq.get("slug", "core_mosquitto")
    if state == "started":
        ok(f"add-on `{slug}` is installed and started")
        return True, hassos_version
    fail(f"add-on `{slug}` is installed but state={state!r}")
    print(f"    Start it from {ha.url}/hassio/addon/{slug}/info")
    return False, hassos_version


def publish_sentinel(ha: HA) -> bool:
    step(5, "Publish a sentinel MQTT message")
    # observatory/status is the firmware's heartbeat topic — round-tripping
    # something onto it is cheap and proves the integration end-to-end.
    # We use a `.test` suffix so the firmware's own heartbeat handler
    # ignores it (the firmware only reads, never validates the schema of
    # outbound topics — but a distinct topic keeps logs clean).
    payload = {
        "scene_id": "ha_setup_check",
        "fps": 0,
        "rssi": 0,
        "uptime_s": int(time.time()),
        "free_heap": 0,
    }
    code, body = ha.post(
        "/api/services/mqtt/publish",
        {
            "topic": "observatory/status.test",
            "payload": json.dumps(payload),
            "retain": False,
        },
    )
    if code in (200, 201):
        ok("mqtt.publish accepted — broker round-trip works.")
        ok("Subscribe with: mosquitto_sub -t 'observatory/#' -v")
        return True
    fail(f"mqtt.publish failed: HTTP {code} — {body!r}")
    if code == 400 and isinstance(body, dict) and "mqtt" in str(body).lower():
        print("    The broker is unreachable from HA. Check the integration's")
        print(f"    health at {ha.url}/config/integrations/integration/mqtt")
    return False


# --- file install + reload + verify + trigger pipeline -----------------

def install_via_ssh(
    ssh_host: str,
    ssh_user: str,
    ssh_port: int,
    ssh_key: str | None,
) -> bool:
    """Copy the package YAML into HA's /config/packages/ over SCP.

    Uses the system `scp` and `ssh` binaries (Windows 10+ ships them;
    macOS/Linux always have them) with public-key auth — the SSH
    add-on must have your public key in its `authorized_keys` config.
    Stdlib-only.

    Default port 22222 is the community SSH add-on; the official
    "Terminal & SSH" add-on uses port 22 — we auto-probe both and
    suggest the right one.
    """
    step(6, f"Install package YAML to {ssh_user}@{ssh_host}:{HA_PACKAGE_DEST}")
    if not HA_PACKAGE_SRC.is_file():
        fail(f"local package file not found: {HA_PACKAGE_SRC}")
        return False
    if shutil.which("scp") is None:
        fail("`scp` binary not found in PATH — install OpenSSH client.")
        return False

    # Pre-flight: TCP-probe the SSH port so we can give a useful error
    # before the user sits through ssh's own 30 s connect timeout.
    import socket
    def _probe(port: int) -> bool:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.settimeout(2.0)
            try:
                return s.connect_ex((ssh_host, port)) == 0
            except OSError:
                return False

    if not _probe(ssh_port):
        fail(f"nothing listening on {ssh_host}:{ssh_port}")
        # Try the other common HA SSH port and tell the user which to use.
        alt = 22 if ssh_port == 22222 else 22222
        if _probe(alt):
            print(f"    But port {alt} IS open — re-run with: --ssh-port {alt}")
            print(f"    (port {alt} is the {'official Terminal & SSH' if alt == 22 else 'community SSH & Web Terminal'} add-on default)")
        else:
            print("    Neither 22 nor 22222 is reachable. Likely causes:")
            print("      • SSH add-on not installed: Settings → Add-ons → Add-on store")
            print("      • SSH add-on installed but not started")
            print("      • SSH add-on started but no authorized_keys configured")
            print("        (the add-on disables sshd until at least one auth is set)")
            print(f"      • Firewall blocking your machine from {ssh_host}")
        return False
    ok(f"port {ssh_port} is open on {ssh_host}")

    # First make sure /config/packages exists. We pipe through ssh because
    # `scp` won't create destination directories.
    ssh_cmd: list[str] = ["ssh", "-p", str(ssh_port), "-o", "StrictHostKeyChecking=accept-new"]
    scp_cmd: list[str] = ["scp", "-P", str(ssh_port), "-o", "StrictHostKeyChecking=accept-new"]
    if ssh_key:
        ssh_cmd += ["-i", ssh_key]
        scp_cmd += ["-i", ssh_key]

    mkdir = subprocess.run(
        ssh_cmd + [f"{ssh_user}@{ssh_host}", "mkdir -p /config/packages"],
        capture_output=True, text=True,
    )
    if mkdir.returncode != 0:
        fail(f"ssh mkdir failed (rc={mkdir.returncode}): {mkdir.stderr.strip()}")
        if "Permission denied" in (mkdir.stderr or "") or "publickey" in (mkdir.stderr or ""):
            print("    Add your public key to the SSH add-on's `authorized_keys`")
            print("    config option (Settings → Add-ons → SSH → Configuration), or")
            print("    pass --ssh-key /path/to/private_key.")
        return False

    cp = subprocess.run(
        scp_cmd + [str(HA_PACKAGE_SRC), f"{ssh_user}@{ssh_host}:{HA_PACKAGE_DEST}"],
        capture_output=True, text=True,
    )
    if cp.returncode != 0:
        fail(f"scp failed (rc={cp.returncode}): {cp.stderr.strip()}")
        return False
    ok(f"copied {HA_PACKAGE_SRC.name} ({HA_PACKAGE_SRC.stat().st_size} bytes)")

    # Verify configuration.yaml has `packages: !include_dir_named packages`
    # under a top-level `homeassistant:` key. Without that wiring HA never
    # loads the package and steps 9–11 will all fail with "missing entity".
    # We grep over SSH (case-insensitive, tolerant of whitespace).
    check = subprocess.run(
        ssh_cmd + [
            f"{ssh_user}@{ssh_host}",
            "grep -E '^[[:space:]]*packages:[[:space:]]*!include_dir_named[[:space:]]+packages'"
            " /config/configuration.yaml || true",
        ],
        capture_output=True, text=True,
    )
    if check.stdout.strip():
        ok("configuration.yaml already wires up packages — nothing to paste.")
    else:
        fail("configuration.yaml is NOT wiring up the packages dir.")
        print()
        print("    Open /config/configuration.yaml and paste THIS at the top")
        print("    (or merge the `packages:` line into your existing")
        print("    `homeassistant:` block):")
        print()
        print("    ┌──────────────────────────────────────────────────")
        print("    │ homeassistant:")
        print("    │   packages: !include_dir_named packages")
        print("    └──────────────────────────────────────────────────")
        print()
        print("    Then re-run this script (it's idempotent).")
        return False
    return True


def check_pyscript(ha: HA) -> bool | None:
    """Verify the pyscript HACS integration is loaded.

    Returns True if loaded, False if missing, None if we couldn't
    tell. The publisher (../pyscript/observatory_publisher.py) needs
    pyscript to run; without it the moon/jupiter/constellation topics
    silently never publish (the YAML stubs that used to cover those
    topics are commented out in the package YAML).
    """
    step("6a", "Verify pyscript integration is loaded")
    code, body = ha.get("/api/config")
    if code != 200 or not isinstance(body, dict):
        warn(f"could not read /api/config (HTTP {code}) — skipping pyscript check.")
        return None
    components = body.get("components", []) or []
    if "pyscript" in components:
        ok("`pyscript` integration is loaded.")
        return True
    fail("`pyscript` integration is NOT loaded.")
    print()
    print("    Pyscript is a HACS custom integration. Install path:")
    print(f"      1. Open  {ha.url}/hacs/integrations")
    print("      2. Search 'Pyscript Python scripting' → Download")
    print("      3. Restart HA")
    print(f"      4. Open  {ha.url}/config/integrations/dashboard")
    print("         → Add Integration → search 'Pyscript' → Submit")
    print("      5. Add this to /config/configuration.yaml (or merge")
    print("         into your existing `pyscript:` block):")
    print()
    print("         ┌──────────────────────────────────────────────────")
    print("         │ pyscript:")
    print("         │   allow_all_imports: true   # needed for skyfield")
    print("         │   hass_is_global: true      # needed for hass.config")
    print("         └──────────────────────────────────────────────────")
    print()
    print("    Without pyscript the observatory/jupiter, /moon and")
    print("    /constellation topics will never publish, and those three")
    print("    firmware scenes will fall back to their on-device defaults")
    print("    (or `WAIT` for jupiter, which has no on-device fallback).")
    return False


def install_publisher_via_ssh(
    ha: HA,
    ssh_host: str,
    ssh_user: str,
    ssh_port: int,
    ssh_key: str | None,
) -> bool:
    """SCP the pyscript publisher + requirements.txt to /config/pyscript/.

    Same SSH/scp transport as install_via_ssh; we re-do the port probe
    so this step is independently runnable. Also calls pyscript.reload
    afterwards so changes take effect without a full HA reload.
    """
    step("6b", f"Install pyscript publisher to {ssh_user}@{ssh_host}:{HA_PYSCRIPT_DIR_DEST}/")
    for src in HA_PYSCRIPT_FILES_SRC:
        if not src.is_file():
            fail(f"local pyscript file not found: {src}")
            return False
    if shutil.which("scp") is None:
        fail("`scp` binary not found in PATH — install OpenSSH client.")
        return False

    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(2.0)
        try:
            if s.connect_ex((ssh_host, ssh_port)) != 0:
                fail(f"nothing listening on {ssh_host}:{ssh_port} (re-run --install first)")
                return False
        except OSError as exc:
            fail(f"could not probe {ssh_host}:{ssh_port}: {exc}")
            return False

    ssh_cmd: list[str] = ["ssh", "-p", str(ssh_port), "-o", "StrictHostKeyChecking=accept-new"]
    scp_cmd: list[str] = ["scp", "-P", str(ssh_port), "-o", "StrictHostKeyChecking=accept-new"]
    if ssh_key:
        ssh_cmd += ["-i", ssh_key]
        scp_cmd += ["-i", ssh_key]

    mkdir = subprocess.run(
        ssh_cmd + [f"{ssh_user}@{ssh_host}", f"mkdir -p {HA_PYSCRIPT_DIR_DEST}"],
        capture_output=True, text=True,
    )
    if mkdir.returncode != 0:
        fail(f"ssh mkdir failed (rc={mkdir.returncode}): {mkdir.stderr.strip()}")
        return False

    # SCP both files in one call — scp accepts multiple sources with a
    # single trailing destination. Less round-trip overhead and the
    # error path is simpler.
    cp = subprocess.run(
        scp_cmd
        + [str(src) for src in HA_PYSCRIPT_FILES_SRC]
        + [f"{ssh_user}@{ssh_host}:{HA_PYSCRIPT_DIR_DEST}/"],
        capture_output=True, text=True,
    )
    if cp.returncode != 0:
        fail(f"scp failed (rc={cp.returncode}): {cp.stderr.strip()}")
        return False
    ok(f"copied {len(HA_PYSCRIPT_FILES_SRC)} file(s) to {HA_PYSCRIPT_DIR_DEST}/")

    # configuration.yaml needs `pyscript:` with allow_all_imports +
    # hass_is_global. Grep for both — print the exact paste block if
    # either is missing. (Same shape as the packages: check earlier.)
    grep_cmds = {
        "pyscript:":          r"^[[:space:]]*pyscript:",
        "allow_all_imports":  r"^[[:space:]]*allow_all_imports:[[:space:]]*true",
        "hass_is_global":     r"^[[:space:]]*hass_is_global:[[:space:]]*true",
    }
    missing: list[str] = []
    for label, pattern in grep_cmds.items():
        check = subprocess.run(
            ssh_cmd + [
                f"{ssh_user}@{ssh_host}",
                f"grep -E '{pattern}' /config/configuration.yaml || true",
            ],
            capture_output=True, text=True,
        )
        if not check.stdout.strip():
            missing.append(label)
    if missing:
        fail(f"configuration.yaml is missing: {', '.join(missing)}")
        print()
        print("    Open /config/configuration.yaml and paste THIS at the top")
        print("    (or merge into your existing `pyscript:` block):")
        print()
        print("    ┌──────────────────────────────────────────────────")
        print("    │ pyscript:")
        print("    │   allow_all_imports: true   # needed for skyfield")
        print("    │   hass_is_global: true      # needed for hass.config")
        print("    └──────────────────────────────────────────────────")
        print()
        print("    Then re-run this script.")
        return False
    ok("configuration.yaml has pyscript + allow_all_imports + hass_is_global.")

    # Pyscript reload picks up new files + re-checks requirements.txt
    # (auto-pip-installs skyfield on first run). Service is registered
    # by the integration, so this only works if --check-pyscript passed.
    code, body = ha.post(
        "/api/services/pyscript/reload", {}, timeout=120.0,
    )
    if code in (200, 201):
        ok("pyscript.reload accepted — skyfield will pip-install on first run.")
        ok("First publish may take 30-60 s while skyfield downloads DE421 (~17 MB).")
        return True
    warn(f"pyscript.reload returned HTTP {code} — file is on disk; restart HA to pick it up.")
    return True


def check_config(ha: HA) -> bool:
    """Ask HA to validate its YAML before reload — catches typos cheaply."""
    step(7, "Validate HA configuration")
    code, body = ha.post("/api/services/homeassistant/check_config", {})
    # Older HA returned 200 with the result body; 2026.x returns 200 with
    # an empty list (action accepted). The follow-up persistent_notification
    # carries the actual result if there's an error — so we instead read
    # the result via the dedicated endpoint when available, and fall back
    # to "call accepted" if not.
    if code in (200, 201):
        ok("HA accepted check_config call (any errors will surface in HA logs).")
        return True
    fail(f"check_config failed: HTTP {code} — {body!r}")
    return False


def reload_yaml(ha: HA) -> bool:
    step(8, "Reload all YAML (this can take 30–120 s on busy installs)")
    # `homeassistant.reload_all` (HA 2023.4+) reloads automations,
    # scripts, scenes, REST sensors, templates, themes, customize, and
    # core config in one shot — exactly what we need after dropping
    # in a packages file.
    #
    # HA holds the HTTP response open until reload_all finishes, which
    # on a fresh install or one with many integrations can run well
    # over a minute. We bump the per-call timeout to 180 s and treat a
    # read timeout as "still running, that's fine" — by the time we
    # poll /api/states a few seconds later the new entities are there.
    try:
        code, body = ha.post(
            "/api/services/homeassistant/reload_all", {}, timeout=180.0,
        )
    except RuntimeError as e:
        fail(f"reload_all could not be dispatched: {e}")
        return False
    except TimeoutError:
        warn("reload_all took longer than 180 s to respond — assuming it's")
        warn("still running server-side. Sleeping 10 s before verifying entities.")
        time.sleep(10.0)
        return True
    if code in (200, 201):
        ok("reload_all completed.")
        # Give HA a moment to register the new entities before we list them.
        time.sleep(2.0)
        return True
    fail(f"reload_all failed: HTTP {code} — {body!r}")
    return False


def verify_automations(ha: HA) -> tuple[bool, dict[str, str]]:
    """Returns (all_present, id_to_entity_id_map).

    HA generates entity_ids from the slugified `alias`, not from the
    `id:` field, so we can't predict them. Instead, we list every
    `automation.*` state and match on its `id` attribute.
    """
    step(9, f"Verify all {len(EXPECTED_AUTOMATION_IDS)} Observatory automations are registered")
    code, body = ha.get("/api/states")
    if code != 200 or not isinstance(body, list):
        fail(f"could not list states (HTTP {code})")
        return False, {}
    found: dict[str, str] = {}  # unique_id -> entity_id
    for s in body:
        if not isinstance(s, dict):
            continue
        eid = s.get("entity_id", "")
        if not eid.startswith("automation."):
            continue
        attrs = s.get("attributes", {}) or {}
        uid = attrs.get("id")
        if uid in EXPECTED_AUTOMATION_IDS:
            found[uid] = eid
    missing = [uid for uid in EXPECTED_AUTOMATION_IDS if uid not in found]
    if not missing:
        ok(f"all {len(EXPECTED_AUTOMATION_IDS)} automations registered:")
        for uid in EXPECTED_AUTOMATION_IDS:
            print(f"      • {uid}  →  {found[uid]}")
        return True, found
    for uid in EXPECTED_AUTOMATION_IDS:
        if uid in found:
            ok(f"{uid}  →  {found[uid]}")
        else:
            fail(f"{uid} — NOT FOUND")
    print("    Did the package file land in /config/packages/ AND is")
    print("    `packages: !include_dir_named packages` in configuration.yaml?")
    print("    Also: did `homeassistant.reload_all` complete? (it can take 1–2 min)")
    return False, found


def trigger_all(ha: HA, found: dict[str, str]) -> bool:
    step(10, "Force-trigger every Observatory automation")
    all_ok = True
    for uid in EXPECTED_AUTOMATION_IDS:
        eid = found.get(uid)
        if not eid:
            fail(f"{uid}: not registered, skipping trigger")
            all_ok = False
            continue
        code, body = ha.post(
            "/api/services/automation/trigger",
            {"entity_id": eid, "skip_condition": True},
        )
        if code in (200, 201):
            ok(f"triggered {eid}")
        else:
            fail(f"trigger {eid} failed: HTTP {code} — {body!r}")
            all_ok = False
    # Give HA a beat so last_triggered timestamps are written before we read.
    time.sleep(1.5)
    return all_ok


def _parse_iso(ts: str) -> datetime | None:
    # HA returns e.g. '2026-05-04T18:23:45.123456+00:00'.
    try:
        return datetime.fromisoformat(ts)
    except (TypeError, ValueError):
        return None


def verify_published(ha: HA, found: dict[str, str], max_age_s: float = 60.0) -> bool:
    """Each automation's `last_triggered` should be within `max_age_s`."""
    step(11, f"Verify each automation fired within the last {int(max_age_s)} s")
    now = datetime.now(timezone.utc)
    all_ok = True
    for uid in EXPECTED_AUTOMATION_IDS:
        eid = found.get(uid)
        if not eid:
            fail(f"{uid}: not registered, can't check")
            all_ok = False
            continue
        code, body = ha.get(f"/api/states/{eid}")
        if code != 200 or not isinstance(body, dict):
            fail(f"{eid}: could not read state (HTTP {code})")
            all_ok = False
            continue
        attrs = body.get("attributes", {}) or {}
        last = _parse_iso(attrs.get("last_triggered", ""))
        if last is None:
            fail(f"{eid}: never triggered (last_triggered = None)")
            all_ok = False
            continue
        age = (now - last).total_seconds()
        if age <= max_age_s:
            ok(f"{eid}: fired {age:.1f}s ago → publish succeeded")
        else:
            fail(f"{eid}: last fired {age:.0f}s ago (> {int(max_age_s)}s window)")
            all_ok = False
    return all_ok


def verify_publisher(ha: HA) -> bool:
    """Force-call each pyscript publisher and confirm it actually published.

    Pyscript auto-exposes every top-level `def` as a service in the
    `pyscript` domain, so we can fire publish_jupiter / _moon /
    _constellation directly — same trick as `automation.trigger` for
    the YAML side.

    Verification reads a witness HA state entity that the publisher
    sets on every successful publish (`pyscript.observatory_publisher_
    <slug>`). We snapshot each entity's `last_changed` BEFORE firing,
    then poll until it advances — proves the publish ran end-to-end
    (skyfield loaded, mqtt.publish accepted, no exception swallowed)
    without depending on /api/error_log (which 404s on some HA
    configurations) or surviving log rotation. First publish can
    take 30–60 s on a cold pyscript install while skyfield
    pip-installs and downloads DE421 (~17 MB), so we poll up to 90 s.
    """
    step("11a", "Force-call pyscript publishers and verify each published")

    # Snapshot the witness entity's last_changed BEFORE firing so we
    # can prove the publish *advanced* it, not just that some old
    # publish happened to be present. Missing entities (first run)
    # snapshot as None — any timestamp counts as progress.
    def _entity_for(topic: str) -> str:
        # Mirror the slug rule in pyscript/observatory_publisher.py
        # _publish(): "observatory/jupiter" → pyscript.observatory_publisher_jupiter
        return "pyscript.observatory_publisher_" + topic.split("/", 1)[1]

    def _last_changed(entity_id: str) -> datetime | None:
        code, body = ha.get(f"/api/states/{entity_id}")
        if code != 200 or not isinstance(body, dict):
            return None
        raw = body.get("last_changed")
        if not isinstance(raw, str):
            return None
        try:
            return datetime.fromisoformat(raw.replace("Z", "+00:00"))
        except ValueError:
            return None

    baseline: dict[str, datetime | None] = {}
    for _, topic in PYSCRIPT_PUBLISHERS:
        baseline[topic] = _last_changed(_entity_for(topic))

    fired: list[tuple[str, str]] = []
    for svc, topic in PYSCRIPT_PUBLISHERS:
        # Pyscript service calls return immediately; the function runs
        # in a worker thread thanks to @pyscript_executor on the
        # compute helpers, so a 30 s skyfield first-load won't block
        # this HTTP call.
        code, body = ha.post(f"/api/services/pyscript/{svc}", {})
        if code in (200, 201):
            ok(f"called pyscript.{svc}")
            fired.append((svc, topic))
        else:
            fail(f"pyscript.{svc} call failed: HTTP {code} — {body!r}")
            if code == 400 and isinstance(body, (str, dict)) and "not found" in str(body).lower():
                print("    Service not registered. Likely causes:")
                print("      • pyscript reload hasn't finished (try again in 30 s)")
                print("      • observatory_publisher.py has a syntax/import error")
                print(f"        — check {ha.url}/config/logs for the traceback")

    if not fired:
        return False

    # Poll the witness entities until each one's last_changed has
    # advanced past the baseline. First-publish skyfield install can
    # take ~60 s; budget 90 s with a gentle backoff.
    pending = {topic for _, topic in fired}
    deadline = time.monotonic() + 90.0
    backoff = 3.0
    while time.monotonic() < deadline and pending:
        time.sleep(backoff)
        for topic in list(pending):
            entity_id = _entity_for(topic)
            current = _last_changed(entity_id)
            if current is None:
                continue
            base = baseline[topic]
            if base is None or current > base:
                ok(f"{topic}: {entity_id} advanced → publish succeeded")
                pending.discard(topic)
        backoff = min(backoff * 1.5, 15.0)

    all_ok = True
    for _, topic in fired:
        if topic in pending:
            entity_id = _entity_for(topic)
            fail(f"{topic}: {entity_id} did not advance within 90 s")
            print("      Likely causes:")
            print("        • skyfield is still pip-installing / downloading DE421")
            print("          (re-run --verify-publisher in a minute)")
            print("        • observer lat/lon falls back to (0,0) — set")
            print("          Settings → System → General → Location")
            print("        • observatory_publisher.py raised before _publish()")
            print(f"          — check {ha.url}/config/logs for the traceback")
            print("        • pyscript publisher not yet redeployed —")
            print("          re-run with --install-publisher")
            all_ok = False
    return all_ok


# --- entry point --------------------------------------------------------

# Search order for the optional .env file (first hit wins). Lets
# folks keep `HA_TOKEN` etc. out of shell history without depending
# on python-dotenv. Format is the standard `KEY=value` per line, with
# `#` comments + blank lines ignored. Values may be wrapped in
# single or double quotes; `\n` / `\\` / `\"` / `\'` inside double
# quotes are unescaped. Existing real env vars always win — the file
# is a *fallback*, never an override (so a one-off
# `HA_TOKEN=... python homeassistant/setup_mqtt.py` still beats
# whatever's in the file).
ENV_FILE_SEARCH_PATHS = [
    Path.cwd() / ".env",                     # current working directory
    Path(__file__).parent / ".env",          # homeassistant/.env
    Path(__file__).parent.parent / ".env",   # repo root .env
]


def _parse_env_line(line: str) -> tuple[str, str] | None:
    """Parse one `KEY=value` line. Returns None for blanks/comments/garbage."""
    s = line.strip()
    if not s or s.startswith("#"):
        return None
    # Tolerate the `export FOO=bar` form people copy out of shell rc files.
    if s.startswith("export "):
        s = s[len("export "):].lstrip()
    if "=" not in s:
        return None
    key, _, raw = s.partition("=")
    key = key.strip()
    if not key or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", key):
        return None
    val = raw.strip()
    # Strip a trailing inline `# comment` only when value is unquoted —
    # quoted values may legitimately contain `#`.
    if val and val[0] not in ("'", '"'):
        hash_idx = val.find(" #")
        if hash_idx >= 0:
            val = val[:hash_idx].rstrip()
    if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
        quote = val[0]
        val = val[1:-1]
        if quote == '"':
            # Minimal escape handling for double-quoted values.
            val = (val.replace(r"\n", "\n")
                      .replace(r"\t", "\t")
                      .replace(r"\\", "\\")
                      .replace(r"\"", "\""))
    return key, val


def load_env_file() -> Path | None:
    """Load the first .env in ENV_FILE_SEARCH_PATHS into os.environ.

    Returns the path that was loaded, or None if no .env was found.
    Real env vars always win — the file only fills in unset keys.
    """
    for path in ENV_FILE_SEARCH_PATHS:
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            continue
        for raw_line in text.splitlines():
            parsed = _parse_env_line(raw_line)
            if parsed is None:
                continue
            key, val = parsed
            os.environ.setdefault(key, val)
        return path
    return None


def main() -> int:
    # Load the .env BEFORE argparse builds its defaults — every flag
    # below uses `os.environ.get(...)` for its default, so the loader
    # has to fire first to be picked up.
    loaded_env = load_env_file()

    ap = argparse.ArgumentParser(
        description="Validate HA + MQTT setup for the Quantum Observatory firmware.",
    )
    ap.add_argument(
        "--url",
        default=os.environ.get("HA_URL"),
        help="Home Assistant base URL (or env HA_URL). e.g. http://homeassistant.local:8123",
    )
    ap.add_argument(
        "--token",
        default=os.environ.get("HA_TOKEN"),
        help="Long-Lived Access Token (or env HA_TOKEN).",
    )
    # --- no-UI pipeline flags ------------------------------------------
    ap.add_argument(
        "--install", action="store_true",
        help="SCP the package YAML to /config/packages/ on the HA host"
             " (requires SSH add-on — see --ssh-* flags).",
    )
    ap.add_argument(
        "--install-publisher", action="store_true",
        help="SCP the pyscript ephemeris publisher (real Jupiter/moon/"
             "constellation data) to /config/pyscript/. Also greps"
             " configuration.yaml for `pyscript:` + allow_all_imports"
             " + hass_is_global and prints the paste block if missing.",
    )
    ap.add_argument(
        "--reload", action="store_true",
        help="Call homeassistant.reload_all after install (or standalone).",
    )
    ap.add_argument(
        "--verify-automations", action="store_true",
        help="Confirm all automation.observatory_* entities exist.",
    )
    ap.add_argument(
        "--trigger-all", action="store_true",
        help="Force-fire every Observatory automation now (skip_condition=true).",
    )
    ap.add_argument(
        "--verify-published", action="store_true",
        help="Read each automation's last_triggered to prove it ran.",
    )
    ap.add_argument(
        "--verify-publisher", action="store_true",
        help="Force-call each pyscript publisher (jupiter/moon/constellation)"
             " and read the witness state entity to confirm each published.",
    )
    ap.add_argument(
        "--all", action="store_true",
        help="Validate → install → install-publisher → reload → verify → trigger → verify-published → verify-publisher.",
    )
    # SSH knobs (only used by --install / --all).
    ap.add_argument("--ssh-host", default=os.environ.get("HA_SSH_HOST"),
                    help="SSH host for --install (defaults to URL host).")
    ap.add_argument("--ssh-user", default=os.environ.get("HA_SSH_USER", "root"),
                    help="SSH user (default: root — the SSH add-on uses root).")
    ap.add_argument("--ssh-port", type=int,
                    default=int(os.environ.get("HA_SSH_PORT", "22")),
                    help="SSH port (default: 22 — official Terminal & SSH add-on."
                         " Use 22222 for the community SSH & Web Terminal add-on).")
    ap.add_argument("--ssh-key", default=os.environ.get("HA_SSH_KEY"),
                    help="Path to private key (default: ssh-agent / ~/.ssh/id_*).")
    args = ap.parse_args()

    if not args.url or not args.token:
        ap.error("both --url and --token are required (or set HA_URL / HA_TOKEN).")

    print(_c("1;37", "Quantum Observatory — Home Assistant setup validator"))
    if loaded_env is not None:
        print(_c("2;37", f"  (loaded env from {loaded_env})"))

    try:
        ha = HA(args.url, args.token)
    except ValueError as e:
        fail(str(e))
        return 1

    try:
        if not check_token(ha):
            return 1
        core_ok, core_version = check_core_version(ha)
        if not core_ok:
            return 2
        integration_ok = check_mqtt_integration(ha)
        addon_state, hassos_version = check_supervisor_and_mosquitto(ha)
        # Only attempt a publish if the integration is loaded — otherwise
        # the service call would 400 with a confusing "service not found".
        publish_ok = publish_sentinel(ha) if integration_ok else False

        # ----- optional no-UI pipeline ---------------------------------
        do_install = args.install or args.all
        do_install_pub = args.install_publisher or args.all
        do_reload = args.reload or args.all
        do_verify_aut = args.verify_automations or args.all
        do_trigger = args.trigger_all or args.all
        do_verify_pub = args.verify_published or args.all
        do_verify_pyscript = args.verify_publisher or args.all

        install_ok: bool | None = None
        pyscript_ok: bool | None = None
        publisher_ok: bool | None = None
        reload_ok: bool | None = None
        automations_ok: bool | None = None
        found_map: dict[str, str] = {}
        trigger_ok: bool | None = None
        published_ok: bool | None = None
        publisher_published_ok: bool | None = None

        if do_install:
            ssh_host = args.ssh_host or re.sub(r"^https?://", "", args.url).split(":")[0].split("/")[0]
            install_ok = install_via_ssh(ssh_host, args.ssh_user, args.ssh_port, args.ssh_key)
        if do_install_pub:
            ssh_host = args.ssh_host or re.sub(r"^https?://", "", args.url).split(":")[0].split("/")[0]
            # Pyscript-integration check is informational — we still SCP
            # the files even if pyscript isn't installed yet, so a fresh
            # install just needs the user to install pyscript via HACS
            # then re-run --reload (or HA restart).
            pyscript_ok = check_pyscript(ha)
            publisher_ok = install_publisher_via_ssh(
                ha, ssh_host, args.ssh_user, args.ssh_port, args.ssh_key,
            )
        if do_reload and (install_ok is not False) and (publisher_ok is not False):
            # check_config is best-effort — don't gate reload on it.
            check_config(ha)
            reload_ok = reload_yaml(ha)
        if do_verify_aut and (reload_ok is not False):
            automations_ok, found_map = verify_automations(ha)
        if do_trigger and automations_ok is not False:
            # If we didn't run --verify-automations this run, populate found_map now.
            if not found_map:
                _, found_map = verify_automations(ha)
            trigger_ok = trigger_all(ha, found_map)
        if do_verify_pub and trigger_ok is not False:
            if not found_map:
                _, found_map = verify_automations(ha)
            published_ok = verify_published(ha, found_map)
        # Pyscript verification is independent of the YAML automations
        # — only gate it on the publisher having been installed (or
        # being already there from a previous run, which we can't tell
        # without an extra round-trip; just attempt and let it fail
        # cleanly if the services aren't registered).
        if do_verify_pyscript and (publisher_ok is not False) and (pyscript_ok is not False):
            publisher_published_ok = verify_publisher(ha)
    except RuntimeError as e:
        print()
        fail(str(e))
        return 1

    print()
    print(_c("1;37", "Summary"))
    print(f"  HA Core version    : {core_version} (need ≥ {fmt_version(MIN_HA_CORE)})")
    if hassos_version:
        host_parsed = parse_version(hassos_version)
        host_ok = host_parsed >= MIN_HA_OS
        print(
            f"  HA OS version      : {hassos_version} (need ≥ {fmt_version(MIN_HA_OS)})"
            + ("" if host_ok else "  ← too old")
        )
    else:
        print("  HA OS version      : n/a (no Supervisor)")
        host_ok = True  # not applicable on Container/Core
    print(f"  integration loaded : {'yes' if integration_ok else 'NO'}")
    print(
        "  mosquitto add-on   : "
        + ("running" if addon_state is True
           else "missing/stopped" if addon_state is False
           else "n/a (no Supervisor)")
    )
    print(f"  publish round-trip : {'ok' if publish_ok else 'FAIL'}")

    def _line(label: str, val: bool | None) -> str:
        if val is None:
            return f"  {label:<19}: skipped"
        return f"  {label:<19}: {'ok' if val else 'FAIL'}"
    if any(v is not None for v in (install_ok, pyscript_ok, publisher_ok, reload_ok, automations_ok, trigger_ok, published_ok, publisher_published_ok)):
        print(_line("package installed",  install_ok))
        print(_line("pyscript loaded",    pyscript_ok))
        print(_line("publisher installed", publisher_ok))
        print(_line("yaml reloaded",      reload_ok))
        print(_line(f"{len(EXPECTED_AUTOMATION_IDS)} automations live", automations_ok))
        print(_line("trigger-all",        trigger_ok))
        print(_line("published verified", published_ok))
        print(_line("publisher verified", publisher_published_ok))

    pipeline_ok = all(
        v in (True, None) for v in (install_ok, publisher_ok, reload_ok, automations_ok, trigger_ok, published_ok, publisher_published_ok)
    )
    all_green = (
        integration_ok
        and publish_ok
        and addon_state is not False
        and host_ok
        and pipeline_ok
    )
    if all_green:
        print()
        print(_c("32", "All green — flash the firmware and watch `observatory/scene` flow."))
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())

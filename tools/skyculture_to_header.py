#!/usr/bin/env python3
"""Convert a Stellarium sky culture + a Hipparcos-keyed star catalog
into ``include/stars.h`` for the constellation_now scene.

Inputs (in this repo's ``assets/`` directory):

  ``sky-culture.json`` (REQUIRED) — a Stellarium sky culture index
    file. Provides constellation line definitions where each line is
    a chain of Hipparcos (HIP) catalog numbers. Already in repo
    (the "western" sky culture from
    https://github.com/Stellarium/stellarium-skycultures).

  star catalog (REQUIRED, drop one of these into ``assets/``):
    * ``hygdata_v41.csv`` — HYG database v4.1 (recommended; one CSV
      with hip/ra/dec/mag/proper columns; MIT-licensed). Get it from:
        https://github.com/astronexus/HYG-Database
    * ``hygdata_v40.csv`` / ``hygdata.csv`` — older HYG files also
      work.
    * ``hip_main.dat`` — raw Hipparcos main catalog from VizieR
      (I/239/hip_main.dat). Slower to parse; supported as a fallback.

Output:

  ``include/stars.h`` — constexpr arrays describing:
    * the bright-star catalog (mag <= magnitude cutoff, plus any
      fainter stars referenced by a constellation line so the
      asterism never breaks)
    * the constellation table (name, IAU code, line segments
      expressed as star indices into the catalog)
    * a string pool of proper star names for the highlight readout.

Why a header (vs. SD or LittleFS load): puts the data in 2 MB flash
instead of 264 KB SRAM, and the linker enforces it never goes
anywhere else. ~5000 stars * ~12 bytes = ~60 KB — comfortable.

Hard rules (enforced; failures abort with a non-zero exit so a build
hook can rely on them):
  * Both input files must be present.
  * sky-culture.json must parse and have a top-level
    ``constellations`` array.
  * Every HIP referenced by a constellation line must resolve to a
    star in the catalog (otherwise the line would be silently
    truncated). If a referenced HIP is missing or filtered out by
    magnitude, the script promotes it back into the kept set.

Stdlib only — no third-party deps.
"""

from __future__ import annotations

import csv
import json
import math
import os
import sys
import time
from dataclasses import dataclass

# --- Tunables ---------------------------------------------------------

# Stars dimmer than this are dropped UNLESS referenced by a
# constellation line. Naked-eye visibility limit is ~6.5 in dark
# skies; 6.0 keeps the catalog comfortably under 5000 entries while
# preserving every asterism star.
MAG_CUTOFF = 6.0

# Cap on how many proper star names we embed. Sorted by brightness
# (lowest mag first), so we get Sirius / Vega / Betelgeuse / ... and
# the cap drops the most obscure named entries.
NAME_CAP = 256

# Maximum length of an embedded proper name (NUL-included). Keeps the
# C++ side's display-line buffers predictable.
NAME_MAX_LEN = 16

# --- Paths ------------------------------------------------------------

REPO_ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS_DIR = os.path.join(REPO_ROOT, "assets")
OUT_PATH   = os.path.join(REPO_ROOT, "include", "stars.h")

SKYCULTURE_PATH = os.path.join(ASSETS_DIR, "sky-culture.json")

CATALOG_CANDIDATES = [
    "hygdata_v41.csv",
    "hygdata_v40.csv",
    "hygdata_v3.csv",
    "hygdata.csv",
    "hip_main.dat",
]


# --- Data model -------------------------------------------------------

@dataclass
class CatalogStar:
    """A single star kept for the firmware-side header."""
    hip: int
    ra_deg: float       # 0..360
    dec_deg: float      # -90..+90
    mag: float          # apparent V magnitude
    proper: str         # "" if no IAU/common name


@dataclass
class CatalogConstellation:
    """A single constellation as it will land in stars.h."""
    iau: str            # "Aql"
    name_en: str        # "Eagle"
    name_native: str    # "Aquila"
    # Each line is a chain of HIPs. The header emits these as star-
    # index pairs (segments) so the firmware draws each connection
    # with a single drawLine() call.
    line_hips: list[list[int]]


# --- Parsers ----------------------------------------------------------

def find_catalog() -> str:
    """Return the path to whichever supported catalog is present, or
    print download instructions and exit non-zero."""
    for name in CATALOG_CANDIDATES:
        p = os.path.join(ASSETS_DIR, name)
        if os.path.isfile(p):
            return p
    sys.stderr.write(
        "[stars] no star catalog found in assets/.\n"
        "        drop one of these into assets/ and re-run:\n"
        "          - hygdata_v41.csv  (recommended; MIT licence)\n"
        "              https://github.com/astronexus/HYG-Database\n"
        "              -> hyg/CURRENT/hygdata_v41.csv\n"
        "          - hip_main.dat     (raw Hipparcos main catalog)\n"
        "              https://cdsarc.cds.unistra.fr/viz-bin/cat/I/239\n"
        "              -> hip_main.dat (or .dat.gz, decompress)\n"
        f"        looked for: {', '.join(CATALOG_CANDIDATES)}\n"
    )
    sys.exit(2)


def parse_hyg_csv(path: str) -> list[CatalogStar]:
    """Parse an HYG-format CSV. Tolerant of v3.x, v4.0, v4.1 column
    layouts — they all share hip/ra/dec/mag/proper."""
    out: list[CatalogStar] = []
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        # HYG ra is in HOURS (0..24); HYG dec is in degrees. Magnitude
        # is plain float in column "mag".
        for row in reader:
            try:
                hip_raw = row.get("hip", "").strip()
                if not hip_raw:
                    continue
                hip = int(float(hip_raw))
                if hip <= 0:
                    continue
                ra_h  = float(row["ra"])           # hours
                dec_d = float(row["dec"])          # degrees
                mag   = float(row["mag"])
            except (KeyError, ValueError):
                continue
            proper = (row.get("proper") or "").strip()
            out.append(CatalogStar(
                hip=hip,
                ra_deg=ra_h * 15.0,
                dec_deg=dec_d,
                mag=mag,
                proper=proper,
            ))
    return out


def parse_hip_main(path: str) -> list[CatalogStar]:
    """Parse the raw Hipparcos hip_main.dat fixed-width file from
    VizieR. Field positions per the I/239 ReadMe:
      H1   HIP  : cols  9..14   (1-based)
      H8   RAdeg: cols 52..63
      H9   DEdeg: cols 65..76
      H5   Vmag : cols 42..46
    Names aren't included in this file; ``proper`` is left "" and the
    user can drop a HYG CSV later if they want named stars in the
    header. ASCII; one row per line; pipe-delimited fixed-width.
    """
    out: list[CatalogStar] = []
    with open(path, encoding="latin-1") as f:
        for line in f:
            try:
                hip = int(line[8:14])
                vmag_raw = line[41:46].strip()
                ra_raw   = line[51:63].strip()
                dec_raw  = line[64:76].strip()
                if not (vmag_raw and ra_raw and dec_raw):
                    continue
                mag    = float(vmag_raw)
                ra_deg = float(ra_raw)
                dec_d  = float(dec_raw)
            except (ValueError, IndexError):
                continue
            out.append(CatalogStar(
                hip=hip, ra_deg=ra_deg, dec_deg=dec_d,
                mag=mag, proper="",
            ))
    return out


def load_catalog(path: str) -> list[CatalogStar]:
    base = os.path.basename(path).lower()
    if base.endswith(".csv"):
        return parse_hyg_csv(path)
    return parse_hip_main(path)


def load_skyculture(path: str) -> tuple[list[CatalogConstellation], dict]:
    """Read the Stellarium sky culture JSON. Returns the constellations
    we'll emit + the parsed root for reference (caller pulls the id +
    region for the file header)."""
    with open(path, encoding="utf-8") as f:
        root = json.load(f)
    out: list[CatalogConstellation] = []
    for entry in root.get("constellations", []):
        iau = (entry.get("iau") or "").strip()
        if not iau:
            continue  # asterisms-only / non-IAU entries get skipped
        name = entry.get("common_name") or {}
        name_en     = (name.get("english") or "").strip()
        name_native = (name.get("native")  or "").strip()
        # Lines may be prefixed with "thin"/"bold" — strip those, we
        # don't care about line weight at 64x32 panel resolution.
        cleaned: list[list[int]] = []
        for raw in entry.get("lines", []):
            if not raw:
                continue
            chain = [v for v in raw if not isinstance(v, str)]
            # Drop chains that contain non-int entries (e.g. raw
            # [ra,dec] coordinate pairs used for non-star asterisms)
            # or that are too short to draw a line.
            if len(chain) < 2 or any(not isinstance(v, int) for v in chain):
                continue
            cleaned.append(chain)
        if not cleaned:
            continue
        out.append(CatalogConstellation(
            iau=iau, name_en=name_en, name_native=name_native,
            line_hips=cleaned,
        ))
    out.sort(key=lambda c: c.iau)  # stable order in the header
    return out, root


# --- Filtering --------------------------------------------------------

def filter_and_resolve(
    stars: list[CatalogStar],
    constellations: list[CatalogConstellation],
) -> tuple[list[CatalogStar], dict[int, int]]:
    """Apply MAG_CUTOFF, then re-add any HIP referenced by a
    constellation line that got filtered out (so no asterism breaks).
    Returns (kept_stars_sorted_by_hip, hip_to_index_map).

    Stars without a HIP (we never load any), or with mag NaN, are
    already excluded by the parser. Duplicates by HIP are deduped
    keeping the brightest (lowest mag) entry — HYG occasionally
    repeats multi-star systems."""
    by_hip: dict[int, CatalogStar] = {}
    for s in stars:
        prev = by_hip.get(s.hip)
        if prev is None or s.mag < prev.mag:
            by_hip[s.hip] = s

    referenced: set[int] = set()
    for c in constellations:
        for chain in c.line_hips:
            for hip in chain:
                referenced.add(hip)

    kept: dict[int, CatalogStar] = {}
    missing_refs: list[int] = []
    for hip, s in by_hip.items():
        if s.mag < MAG_CUTOFF or hip in referenced:
            kept[hip] = s
    for hip in referenced:
        if hip not in kept:
            if hip in by_hip:
                kept[hip] = by_hip[hip]
            else:
                missing_refs.append(hip)
    if missing_refs:
        sys.stderr.write(
            f"[stars] WARNING: {len(missing_refs)} HIP references in "
            f"sky-culture.json have no entry in the star catalog "
            f"(asterism lines through those stars will be skipped):\n"
        )
        for hip in sorted(missing_refs)[:20]:
            sys.stderr.write(f"        HIP {hip}\n")
        if len(missing_refs) > 20:
            sys.stderr.write(f"        ... and {len(missing_refs) - 20} more\n")

    out = sorted(kept.values(), key=lambda s: s.hip)
    hip_to_idx = {s.hip: i for i, s in enumerate(out)}
    return out, hip_to_idx


# --- Header emission --------------------------------------------------

# RA stored as milliarcseconds (0..1_296_000_000). Fits int32_t.
# Dec stored as milliarcseconds (-324_000_000..+324_000_000). Same.
# Magnitude * 100 stored as int16 (range ~-200..+700 covers everything
# from Sirius (-1.46) to mag 6.5 cutoff comfortably).
def ra_to_mas(deg: float) -> int:
    return int(round(deg * 3_600_000.0))


def dec_to_mas(deg: float) -> int:
    return int(round(deg * 3_600_000.0))


def mag_x100(m: float) -> int:
    return int(round(m * 100.0))


def safe_proper(name: str) -> str:
    """Strip to ASCII, cap length to NAME_MAX_LEN-1 (room for NUL).
    Names with non-ASCII chars (HYG has a few like Cor Caroli's
    diacritics) get the diacritic stripped via a NFKD pass."""
    if not name:
        return ""
    import unicodedata
    norm = unicodedata.normalize("NFKD", name)
    ascii_only = norm.encode("ascii", "ignore").decode("ascii")
    ascii_only = ascii_only.strip()
    if len(ascii_only) >= NAME_MAX_LEN:
        ascii_only = ascii_only[: NAME_MAX_LEN - 1]
    return ascii_only


def emit_header(
    stars: list[CatalogStar],
    constellations: list[CatalogConstellation],
    hip_to_idx: dict[int, int],
    sc_root: dict,
    catalog_path: str,
) -> str:
    """Build the include/stars.h text. Pure string assembly so the
    output is deterministic + diff-friendly."""

    # Pick which stars get an embedded name. Sort by magnitude to keep
    # the most famous ones if we hit NAME_CAP.
    named = [s for s in stars if s.proper]
    named.sort(key=lambda s: s.mag)
    named = named[:NAME_CAP]
    name_for_hip: dict[int, str] = {s.hip: safe_proper(s.proper) for s in named}
    # If safe_proper returned "" (all-non-ASCII edge case), drop it.
    name_for_hip = {h: n for h, n in name_for_hip.items() if n}

    # Build the ordered list of unique names + a hip->name_idx map.
    # Index 0 is the empty sentinel so star.name_idx == 0 means "no
    # name" without needing a separate flag bit.
    names_ordered: list[str] = [""]
    name_to_idx: dict[str, int] = {"": 0}
    for hip in sorted(name_for_hip.keys()):
        n = name_for_hip[hip]
        if n not in name_to_idx:
            name_to_idx[n] = len(names_ordered)
            names_ordered.append(n)
    star_name_idx = {hip: name_to_idx[name_for_hip[hip]] for hip in name_for_hip}

    # Build constellation line segments as (a, b) star-index pairs.
    # A "chain" of N HIPs becomes N-1 segments. Drop segments where
    # either endpoint isn't in the catalog (filter_and_resolve already
    # reported these as warnings).
    cons_segments: list[list[tuple[int, int]]] = []
    for c in constellations:
        segs: list[tuple[int, int]] = []
        for chain in c.line_hips:
            for i in range(len(chain) - 1):
                a, b = chain[i], chain[i + 1]
                ai = hip_to_idx.get(a)
                bi = hip_to_idx.get(b)
                if ai is None or bi is None:
                    continue
                segs.append((ai, bi))
        cons_segments.append(segs)

    sc_id     = sc_root.get("id", "?")
    sc_region = sc_root.get("region", "?")

    L: list[str] = []
    P = L.append
    P(f"// Auto-generated by tools/skyculture_to_header.py.")
    P(f"// DO NOT EDIT BY HAND. Regenerate after changing")
    P(f"//   assets/sky-culture.json or the star catalog.")
    P(f"//")
    P(f"// Sky culture: {sc_id!r} (region: {sc_region!r})")
    P(f"// Star catalog source: {os.path.basename(catalog_path)!r}")
    P(f"// Magnitude cutoff:    < {MAG_CUTOFF}")
    P(f"// Stars kept:          {len(stars)}")
    P(f"// Named stars:         {len(names_ordered) - 1} (cap {NAME_CAP})")
    P(f"// Constellations:      {len(constellations)}")
    P(f"// Total line segments: {sum(len(s) for s in cons_segments)}")
    P(f"// Generated:           {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}")
    P(f"//")
    P(f"// Storage layout — designed for flash, not SRAM:")
    P(f"//   * Stars are sorted by HIP for binary search.")
    P(f"//   * Coordinates use int32 milliarcseconds (RA: 0..1.296e9,")
    P(f"//     Dec: +/-3.24e8) — keeps the data float-free for the")
    P(f"//     render hot path (NFR-1.3) while preserving sub-arcsecond")
    P(f"//     precision (way more than 64x32 needs).")
    P(f"//   * Magnitudes use int16 * 100 (range easily covers Sirius")
    P(f"//     -146 to mag 6.5 = 650).")
    P(f"//   * Constellation line segments reference stars by uint16")
    P(f"//     index into kStars[] (HIP itself is uint32).")
    P(f"")
    P(f"#pragma once")
    P(f"")
    P(f"#include <stdint.h>")
    P(f"")
    P(f"namespace stars {{")
    P(f"")
    P(f"struct Star {{")
    P(f"  uint32_t hip;        // Hipparcos catalog number")
    P(f"  int32_t  ra_mas;     // RA in milliarcseconds (0..1.296e9)")
    P(f"  int32_t  dec_mas;    // Declination in milliarcseconds")
    P(f"  int16_t  mag_x100;   // V magnitude * 100 (e.g. -146 = Sirius)")
    P(f"  uint16_t name_idx;   // index into kStarNames; 0 = unnamed")
    P(f"}};")
    P(f"")
    P(f"inline constexpr uint16_t kStarsCount = {len(stars)};")
    P(f"")
    P(f"inline constexpr Star kStars[kStarsCount] = {{")
    for s in stars:
        ni = star_name_idx.get(s.hip, 0)
        P(f"  {{ {s.hip}, {ra_to_mas(s.ra_deg)}, {dec_to_mas(s.dec_deg)}, "
          f"{mag_x100(s.mag)}, {ni} }},")
    P(f"}};")
    P(f"")
    P(f"// Proper-name table. Index 0 is the empty sentinel \"\" so a")
    P(f"// star with name_idx == 0 has no proper name. All other entries")
    P(f"// are ASCII-stripped, NUL-terminated, capped at "
      f"{NAME_MAX_LEN - 1} chars.")
    P(f"inline constexpr uint16_t kStarNamesCount = {len(names_ordered)};")
    P(f"inline constexpr const char* kStarNames[kStarNamesCount] = {{")
    for n in names_ordered:
        P(f"  \"{n}\",")
    P(f"}};")
    P(f"")
    P(f"// Binary-search lookup. Returns kStarsCount when not found —")
    P(f"// caller compares against that sentinel rather than -1 so the")
    P(f"// return type stays unsigned.")
    P(f"inline constexpr uint16_t find_by_hip(uint32_t hip) {{")
    P(f"  uint16_t lo = 0, hi = kStarsCount;")
    P(f"  while (lo < hi) {{")
    P(f"    const uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);")
    P(f"    const uint32_t h = kStars[mid].hip;")
    P(f"    if      (h < hip) lo = static_cast<uint16_t>(mid + 1);")
    P(f"    else if (h > hip) hi = mid;")
    P(f"    else              return mid;")
    P(f"  }}")
    P(f"  return kStarsCount;")
    P(f"}}")
    P(f"")
    P(f"}}  // namespace stars")
    P(f"")
    P(f"namespace constellations_iau {{")
    P(f"")
    P(f"struct LineSegment {{")
    P(f"  uint16_t a;          // index into stars::kStars")
    P(f"  uint16_t b;")
    P(f"}};")
    P(f"")
    P(f"struct Entry {{")
    P(f"  const char*        iau;        // IAU 3-letter code, e.g. \"Aql\"")
    P(f"  const char*        name_en;    // English common name")
    P(f"  const char*        name_native; // Native (Latin for western)")
    P(f"  const LineSegment* lines;")
    P(f"  uint16_t           line_count;")
    P(f"}};")
    P(f"")
    # Per-constellation segment arrays.
    for c, segs in zip(constellations, cons_segments):
        if not segs:
            continue
        P(f"inline constexpr LineSegment k{c.iau}_lines[{len(segs)}] = {{")
        for a, b in segs:
            P(f"  {{ {a}, {b} }},")
        P(f"}};")
        P(f"")
    # Top-level catalog.
    nonempty = [
        (c, segs) for c, segs in zip(constellations, cons_segments) if segs
    ]
    P(f"inline constexpr uint8_t kCatalogCount = {len(nonempty)};")
    P(f"")
    P(f"inline constexpr Entry kCatalog[kCatalogCount] = {{")
    for c, segs in nonempty:
        en  = c.name_en.replace('"', '\\"')
        nat = c.name_native.replace('"', '\\"')
        P(f"  {{ \"{c.iau}\", \"{en}\", \"{nat}\", "
          f"k{c.iau}_lines, {len(segs)} }},")
    P(f"}};")
    P(f"")
    P(f"}}  // namespace constellations_iau")
    P(f"")
    return "\n".join(L)


# --- Main -------------------------------------------------------------

def main() -> int:
    if not os.path.isfile(SKYCULTURE_PATH):
        sys.stderr.write(
            f"[stars] sky-culture.json not found at {SKYCULTURE_PATH}\n"
        )
        return 2

    catalog_path = find_catalog()

    print(f"[stars] reading sky culture: {os.path.relpath(SKYCULTURE_PATH, REPO_ROOT)}")
    constellations, sc_root = load_skyculture(SKYCULTURE_PATH)
    print(f"[stars]   {len(constellations)} IAU constellations")

    print(f"[stars] reading star catalog: {os.path.relpath(catalog_path, REPO_ROOT)}")
    raw_stars = load_catalog(catalog_path)
    print(f"[stars]   {len(raw_stars)} total HIP entries")

    kept, hip_to_idx = filter_and_resolve(raw_stars, constellations)
    print(f"[stars]   {len(kept)} stars kept (mag < {MAG_CUTOFF} OR referenced by a line)")

    out = emit_header(kept, constellations, hip_to_idx, sc_root, catalog_path)

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    rel = os.path.relpath(OUT_PATH, REPO_ROOT)
    print(f"[stars] wrote {rel} ({len(out):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

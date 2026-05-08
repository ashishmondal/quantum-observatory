#!/usr/bin/env python3
"""Batch-convert TTFs to Adafruit_GFX headers from a TOML manifest.

Reads `assets/fonts.toml` (or a `--config` override) and invokes
`tools/convert_font.sh` once per `[[font]]` entry. Single-shot
conversions can still use `tools/convert_font.sh` directly — this
script just batches it from a declarative manifest so the set of
bundled fonts is one obvious file to edit (PLAN T.6).

Exits non-zero on the first failed conversion (no partial-success
fan-out — a missing TTF or a broken glyph range is a hard error
worth surfacing immediately rather than buried in a summary).

Usage:
    tools/convert_fonts.py                       # convert every entry
    tools/convert_fonts.py apollo_header         # convert only entries
                                                 # whose `id` matches
    tools/convert_fonts.py --config path.toml    # alternate manifest
    tools/convert_fonts.py --dry-run             # print what would run

Schema validated minimally — anything beyond required fields is
caller error and gets a precise message, never a Python traceback.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = REPO_ROOT / "assets" / "fonts.toml"
CONVERTER = REPO_ROOT / "tools" / "convert_font.sh"

REQUIRED_FIELDS = ("id", "input", "size", "output")
OPTIONAL_FIELDS = ("first", "last", "note")
ALLOWED_FIELDS = set(REQUIRED_FIELDS) | set(OPTIONAL_FIELDS)


@dataclass
class FontEntry:
    """One row from the manifest, post-validation."""
    id: str
    input: Path     # absolute
    size: int
    output: Path    # absolute
    first: int
    last: int
    note: str


def _load(config_path: Path) -> list[FontEntry]:
    """Parse the TOML manifest and validate every entry.

    Validation surfaces the offending entry's `id` (or its index when
    the id itself is missing) so error messages point straight at the
    line to fix in fonts.toml.
    """
    if not config_path.is_file():
        sys.exit(f"error: config not found: {config_path}")

    with config_path.open("rb") as fh:
        data = tomllib.load(fh)

    rows = data.get("font")
    if not isinstance(rows, list) or not rows:
        sys.exit(f"error: {config_path} has no [[font]] entries")

    entries: list[FontEntry] = []
    seen_ids: set[str] = set()
    seen_outputs: set[Path] = set()

    for index, row in enumerate(rows):
        label = row.get("id", f"#{index}")

        unknown = set(row) - ALLOWED_FIELDS
        if unknown:
            sys.exit(f"error: [{label}] unknown fields: {sorted(unknown)}")

        for field in REQUIRED_FIELDS:
            if field not in row:
                sys.exit(f"error: [{label}] missing required field '{field}'")

        ent_id = str(row["id"]).strip()
        if not ent_id:
            sys.exit(f"error: entry #{index} has empty id")
        if ent_id in seen_ids:
            sys.exit(f"error: duplicate id '{ent_id}'")
        seen_ids.add(ent_id)

        size = row["size"]
        if not isinstance(size, int) or size <= 0:
            sys.exit(f"error: [{ent_id}] size must be a positive int")

        first = row.get("first", 32)
        last = row.get("last", 126)
        if not (isinstance(first, int) and isinstance(last, int)
                and 0 <= first <= last <= 0xFFFF):
            sys.exit(f"error: [{ent_id}] invalid first/last range "
                     f"(got first={first}, last={last})")

        in_path = (REPO_ROOT / row["input"]).resolve()
        out_path = (REPO_ROOT / row["output"]).resolve()

        if out_path in seen_outputs:
            sys.exit(f"error: [{ent_id}] output collides with another entry: "
                     f"{out_path.relative_to(REPO_ROOT)}")
        seen_outputs.add(out_path)

        entries.append(FontEntry(
            id=ent_id,
            input=in_path,
            size=size,
            output=out_path,
            first=first,
            last=last,
            note=str(row.get("note", "")),
        ))

    return entries


def _run_one(ent: FontEntry, dry_run: bool) -> int:
    """Invoke convert_font.sh for one entry; return its exit code."""
    cmd = [
        str(CONVERTER),
        str(ent.input),
        str(ent.size),
        str(ent.output),
        str(ent.first),
        str(ent.last),
    ]
    print(f"→ {ent.id}: {shlex.join(cmd)}")
    if dry_run:
        return 0
    if not ent.input.is_file():
        print(f"  ✗ input missing: {ent.input.relative_to(REPO_ROOT)}",
              file=sys.stderr)
        return 1
    proc = subprocess.run(cmd, cwd=REPO_ROOT)
    return proc.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ids", nargs="*",
                        help="optional font ids to convert (default: all)")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                        help=f"TOML manifest path (default: "
                             f"{DEFAULT_CONFIG.relative_to(REPO_ROOT)})")
    parser.add_argument("--dry-run", action="store_true",
                        help="print commands without running them")
    args = parser.parse_args()

    if not os.access(CONVERTER, os.X_OK):
        sys.exit(f"error: {CONVERTER.relative_to(REPO_ROOT)} is not executable")

    entries = _load(args.config)

    if args.ids:
        wanted = set(args.ids)
        entries = [e for e in entries if e.id in wanted]
        missing = wanted - {e.id for e in entries}
        if missing:
            sys.exit(f"error: no such font id(s): {sorted(missing)}")

    if not entries:
        print("(nothing to do — all entries filtered out)")
        return 0

    print(f"converting {len(entries)} font(s) "
          f"from {args.config.relative_to(REPO_ROOT)}\n")

    for ent in entries:
        rc = _run_one(ent, args.dry_run)
        if rc != 0:
            sys.exit(f"error: [{ent.id}] convert_font.sh failed (exit {rc})")

    print(f"\n✓ converted {len(entries)} font(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

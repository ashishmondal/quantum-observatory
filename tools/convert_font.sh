#!/usr/bin/env bash
# Convert a TTF/OTF to an Adafruit_GFX-compatible GFXfont header.
#
# Usage:
#   tools/convert_font.sh <input.ttf> <pt_size> <output.h> [first_char] [last_char]
#
#   first_char / last_char default to 32 (space) and 126 (~) — printable
#   ASCII. Pass higher last_char if you want to include glyphs above
#   0x7E (e.g. for the THEME.md §9 custom glyph plan).
#
# Examples:
#   tools/convert_font.sh assets/fonts/PressStart2P.ttf 8 \
#       include/fonts/press_start_2p_8pt7b.h
#
#   tools/convert_font.sh assets/fonts/VT323.ttf 8 \
#       include/fonts/vt323_8pt7b.h 32 126
#
# Builds tools/fontconvert/fontconvert on first use (one-time gcc + FreeType).
# Writes the converted header to the requested path.

set -euo pipefail

if [[ $# -lt 3 || $# -gt 5 ]]; then
  echo "usage: $0 <input.ttf> <pt_size> <output.h> [first_char] [last_char]" >&2
  exit 64
fi

INPUT="$1"
SIZE="$2"
OUTPUT="$3"
FIRST="${4:-32}"
LAST="${5:-126}"

if [[ ! -f "$INPUT" ]]; then
  echo "error: input font not found: $INPUT" >&2
  exit 66
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONVERTER="$REPO_ROOT/tools/fontconvert/fontconvert"

if [[ ! -x "$CONVERTER" ]]; then
  echo "→ building fontconvert (one-time)..."
  make -C "$REPO_ROOT/tools/fontconvert" >/dev/null
fi

mkdir -p "$(dirname "$OUTPUT")"

# fontconvert emits non-`static` table definitions, which causes
# multiple-definition link errors when the header is #included from
# more than one TU. Post-process so each header is self-contained:
#   - prepend `#pragma once`
#   - convert table definitions (`const uint8_t .*Bitmaps[]`,
#     `const GFXglyph .*Glyphs[]`, `const GFXfont .*`) to `static const`
# Same fixup the existing `digital_7__mono_14pt7b.h` uses (PLAN T.2).
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

"$CONVERTER" "$INPUT" "$SIZE" "$FIRST" "$LAST" \
  | sed -E 's/^const (uint8_t|GFXglyph|GFXfont) /static const \1 /' \
  > "$TMP"

{
  echo "#pragma once"
  echo
  cat "$TMP"
} > "$OUTPUT"

LINES=$(wc -l < "$OUTPUT")
BYTES=$(wc -c < "$OUTPUT")
echo "✓ wrote $OUTPUT ($LINES lines, $BYTES bytes)"

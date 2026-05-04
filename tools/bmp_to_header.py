#!/usr/bin/env python3
"""Convert assets/*.bmp into palette-indexed C++ headers for cyclable BG.

For each `assets/<name>.bmp` we emit `include/bitmaps/<name>.h` containing:

    inline constexpr uint16_t k<Name>Palette[192];   // RGB565
    inline constexpr uint8_t  k<Name>Pixels[64*32];  // indices 0..191
    inline constexpr BitmapBg::Region k<Name>Regions[N];  // optional

Hard rules (enforced by the converter; failures abort the build):
  * 8-bit indexed BMP, uncompressed (BI_RGB).
  * Exactly 64 x 32 pixels.
  * Every pixel must reference palette entries 0..191 only. Entries
    192..255 are reserved by the firmware's split-palette layout
    (see include/color_palette.h: BG region is 0..191, FG is 192..255).
    Remap your image's palette before export if it uses entries beyond
    191.

Optional sidecar `assets/<name>.regions` declares cycling sub-ranges of
the BG palette for this image. One region per line:

    start length speed

`speed` is signed integer steps/sec (+ forward, - backward, 0 = static).
Regions must not overlap and must stay within 0..191. If the sidecar is
absent, a single static region covering the used palette range is
emitted (no animation).

Always emits include/bitmaps/_index.h with a registry of all images.
Stdlib only — no Pillow / numpy required.
"""

import pathlib
import struct
import sys

ROOT   = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
OUT    = ROOT / "include" / "bitmaps"

PANEL_W, PANEL_H = 64, 32
BG_LIMIT         = 192   # must match palette::BG_LEN in color_palette.h


class BmpError(Exception):
    pass


def parse_bmp(path: pathlib.Path):
    """Return (palette[256] of (r,g,b), pixels[2048] of int 0..255)."""
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise BmpError("not a BMP (missing 'BM' signature)")

    pixel_off   = struct.unpack_from("<I", data, 10)[0]
    dib_size    = struct.unpack_from("<I", data, 14)[0]
    width       = struct.unpack_from("<i", data, 18)[0]
    height      = struct.unpack_from("<i", data, 22)[0]
    bpp         = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]

    if bpp != 8:
        raise BmpError(f"must be 8-bit indexed (got {bpp}-bit)")
    if compression != 0:
        raise BmpError("must be uncompressed (BI_RGB)")
    if width != PANEL_W or abs(height) != PANEL_H:
        raise BmpError(
            f"must be {PANEL_W}x{PANEL_H} (got {width}x{abs(height)})")

    palette_off = 14 + dib_size
    palette = []
    for i in range(256):
        b, g, r, _ = data[palette_off + i*4 : palette_off + i*4 + 4]
        palette.append((r, g, b))

    bottom_up  = height > 0
    h          = abs(height)
    row_stride = ((width * bpp + 31) // 32) * 4

    pixels = [0] * (PANEL_W * PANEL_H)
    for row in range(h):
        src_y   = (h - 1 - row) if bottom_up else row
        row_off = pixel_off + src_y * row_stride
        for x in range(width):
            pixels[row * PANEL_W + x] = data[row_off + x]
    return palette, pixels


def rgb_to_565(r: int, g: int, b: int) -> int:
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5


def parse_regions_sidecar(path: pathlib.Path):
    """Return list of (start, length, speed) tuples or [] if missing."""
    if not path.exists():
        return []
    out = []
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3:
            raise BmpError(
                f"{path.name}:{lineno}: expected 'start length speed', "
                f"got {len(parts)} fields")
        try:
            start  = int(parts[0])
            length = int(parts[1])
            speed  = int(parts[2])
        except ValueError as e:
            raise BmpError(f"{path.name}:{lineno}: {e}") from None
        out.append((start, length, speed))

    # Validate ranges + no overlap.
    out_sorted = sorted(out, key=lambda r: r[0])
    prev_end = 0
    for start, length, speed in out_sorted:
        if start < 0 or length <= 0:
            raise BmpError(
                f"{path.name}: region start={start} length={length} invalid")
        if start + length > BG_LIMIT:
            raise BmpError(
                f"{path.name}: region {start}+{length} exceeds BG limit "
                f"{BG_LIMIT}")
        if start < prev_end:
            raise BmpError(
                f"{path.name}: region {start}+{length} overlaps previous")
        prev_end = start + length
    return out


def safe_basename(stem: str) -> str:
    return "".join(c if c.isalnum() else "_" for c in stem)


def to_pascal(name: str) -> str:
    return "".join(p.capitalize() for p in name.split("_") if p)


def emit_image_header(stem: str, palette, pixels, regions):
    safe = safe_basename(stem)
    base = "k" + to_pascal(safe)
    sym_palette = base + "Palette"
    sym_pixels  = base + "Pixels"
    sym_regions = base + "Regions"

    OUT.mkdir(parents=True, exist_ok=True)
    out = OUT / f"{safe}.h"

    lines = [
        f"// Auto-generated from assets/{stem}.bmp by tools/bmp_to_header.py.",
        "// DO NOT EDIT BY HAND.",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        '#include "backgrounds/bitmap_bg.h"',
        "",
        "// 192-entry RGB565 palette (BG region only). Unused entries are",
        "// padded with black so the table is always exactly 192 long.",
        f"inline constexpr uint16_t {sym_palette}[192] = {{",
    ]
    for i in range(0, BG_LIMIT, 16):
        chunk = palette[i : i + 16]
        line  = "  " + " ".join(f"0x{rgb_to_565(*c):04X}," for c in chunk)
        lines.append(line)
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr uint8_t {sym_pixels}[{PANEL_W} * {PANEL_H}] = {{")
    for y in range(PANEL_H):
        row = "  " + " ".join(
            f"{pixels[y * PANEL_W + x]:3d}," for x in range(PANEL_W)
        )
        lines.append(row)
    lines.append("};")
    lines.append("")

    if regions:
        lines.append(f"inline constexpr BitmapBg::Region {sym_regions}[] = {{")
        for start, length, speed in regions:
            lines.append(
                f"  {{ /*start=*/{start:3d}, /*length=*/{length:3d}, "
                f"/*speed=*/{speed:+5d} }},")
        lines.append("};")
        lines.append(
            f"inline constexpr uint8_t {sym_regions}Count = "
            f"sizeof({sym_regions}) / sizeof({sym_regions}[0]);")
    else:
        # Single static region covering the used range. `Pixels`'s max
        # is captured at codegen time so the C++ side never has to scan.
        used_max = max(pixels)
        lines.append(
            f"inline constexpr BitmapBg::Region {sym_regions}[] = {{ "
            f"{{ 0, {used_max + 1}, 0 }} }};")
        lines.append(f"inline constexpr uint8_t {sym_regions}Count = 1;")
    lines.append("")
    out.write_text("\n".join(lines))
    return out, sym_palette, sym_pixels, sym_regions


def emit_registry(entries):
    OUT.mkdir(parents=True, exist_ok=True)
    out = OUT / "_index.h"
    lines = [
        "// Auto-generated by tools/bmp_to_header.py.",
        "// DO NOT EDIT BY HAND.",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        '#include "backgrounds/bitmap_bg.h"',
        "",
    ]
    for stem, *_ in entries:
        lines.append(f'#include "bitmaps/{safe_basename(stem)}.h"')
    if entries:
        lines.append("")
    lines += [
        "struct ImageEntry {",
        "  const char*           name;",
        "  const uint16_t*       palette;",
        "  const uint8_t*        pixels;",
        "  const BitmapBg::Region* regions;",
        "  uint8_t               region_count;",
        "};",
        "",
        "inline constexpr ImageEntry kImageRegistry[] = {",
    ]
    for stem, sym_palette, sym_pixels, sym_regions in entries:
        lines.append(
            f'  {{ "{stem}", {sym_palette}, {sym_pixels}, '
            f'{sym_regions}, {sym_regions}Count }},')
    if not entries:
        lines.append("  { nullptr, nullptr, nullptr, nullptr, 0 },")
    lines.append("};")
    lines.append("")
    lines.append(
        f"inline constexpr int kImageRegistryCount = {len(entries)};")
    lines.append("")
    out.write_text("\n".join(lines))
    return out


def main() -> int:
    bmps = sorted(ASSETS.glob("*.bmp")) if ASSETS.exists() else []
    entries = []
    for p in bmps:
        try:
            palette, pixels = parse_bmp(p)
            # Hard-enforce the 192-index rule.
            bad = [px for px in pixels if px >= BG_LIMIT]
            if bad:
                raise BmpError(
                    f"pixels reference palette entries >= {BG_LIMIT} "
                    f"(found indices {sorted(set(bad))[:5]}...). "
                    f"Remap to entries 0..{BG_LIMIT - 1} only.")
            regions = parse_regions_sidecar(
                p.with_suffix(".regions"))
        except BmpError as e:
            print(f"[bmp] ERROR {p.name}: {e}", file=sys.stderr)
            return 1
        out, sp, sx, sr = emit_image_header(p.stem, palette, pixels, regions)
        print(f"[bmp] {p.name} -> {out.relative_to(ROOT)}  "
              f"({len(regions) if regions else 1} region(s))")
        entries.append((p.stem, sp, sx, sr))

    idx = emit_registry(entries)
    if entries:
        print(f"[bmp] wrote {idx.relative_to(ROOT)}  ({len(entries)} image(s))")
    else:
        print(f"[bmp] no .bmp files in assets/; wrote empty registry")
    return 0


if __name__ == "__main__":
    sys.exit(main())

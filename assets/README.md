# Image assets

Drop **64×32, 8-bit indexed, uncompressed** `.bmp` files in this folder.
On the next `pio run` the pre-build step ([../tools/bmp_to_header.py](../tools/bmp_to_header.py))
auto-generates `include/bitmaps/<name>.h` containing an
`inline constexpr uint16_t k<Name>[64 * 32]` array of RGB565 values
(panel-native), plus an `_index.h` registry so the firmware can find them.

Show one over MQTT:

```text
observatory/scene  {"scene_id":"bg_image","priority":3,"sticky":true}
```

Today the `bg_image` scene always shows the **first** registered image.
A `image_id` selector can be added later when there's more than one.

## Authoring tips

- Most tools save 24-bit by default. In **GIMP**: Image → Mode → Indexed
  (max 256 colors), then Export As `.bmp`, advanced options → 8 bpp.
- In **Photoshop**: Image → Mode → Indexed Color, Save As BMP, depth 8-bit.
- The exact dimensions matter — the converter rejects anything that isn't
  64×32 to keep the firmware path branch-free. Resize before export.
- Compression must be off (`BI_RGB`). Most tools do this by default for
  8-bit BMPs.

## Theming (FR-15.6)

Images are **themable by default**: the firmware retones them at runtime
when a non-default theme is active by mapping each palette entry's
luminance through a 4-stop ramp the theme declares. The default theme
(`apollo_amber`) passes through unchanged. See [../docs/THEME.md](../docs/THEME.md) §6.

Implications for authoring:

- Author in whatever colors you want. The default-theme look is exactly
  what you put in the BMP.
- For best results under non-default themes, make sure your image's
  **luminance** reads well — a green-CRT or amber-phosphor retoning
  preserves brightness relationships, not hues. A quick sanity check:
  desaturate the image to grayscale; if it still reads, it will retone
  cleanly.
- To opt a single image out of retoning entirely (it always shows in its
  original palette regardless of theme), drop an empty sidecar file:
  `assets/<name>.notheme`. Use this only when color carries information
  the theme would erase.

## Star catalog (constellation_now scene)

The constellation scene's data is generated from two files:

- **`sky-culture.json`** — Stellarium "western" sky culture (committed).
  Provides constellation line definitions as chains of Hipparcos (HIP)
  catalog numbers. Source:
  <https://github.com/Stellarium/stellarium-skycultures> (`western/index.json`).
- **`hygdata_v41.csv`** — HYG database v4.1 (NOT committed; ~32 MB,
  gitignored). Provides per-HIP RA/Dec/magnitude/proper-name lookup.
  Download once with:

  ```sh
  curl -L -o assets/hygdata_v41.csv \
    https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/CURRENT/hygdata_v41.csv
  ```

  MIT-licensed. Source: <https://github.com/astronexus/HYG-Database>.
  The converter also accepts `hygdata_v40.csv`, `hygdata_v3.csv`,
  `hygdata.csv`, or raw VizieR `hip_main.dat` as fallbacks.

Run [../tools/skyculture_to_header.py](../tools/skyculture_to_header.py)
to regenerate `include/stars.h` (currently ~5000 stars at mag < 6.0,
auto-promoting any fainter HIPs referenced by an asterism so no
constellation line breaks).

